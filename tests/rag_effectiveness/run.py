#!/usr/bin/env python3
"""One-command build, offline validation, or live paired RAG evaluation (stdlib only)."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

from dataset import cases, catalogs, synthetic_tools
from evaluation import aggregate, console, coverage, mean, report, score

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DEFAULT_CHAT = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
DEFAULT_EMBEDDING = "https://dashscope.aliyuncs.com/api/v1/services/embeddings/text-embedding/text-embedding"

# 最近一次评测的内存结果，仅供自检断言使用；不落盘。
LAST_RUN = {}


class Bridge:
    def __init__(self, executable, log_dir):
        Path(log_dir).mkdir(parents=True, exist_ok=True)
        self.log = (Path(log_dir) / "backend.log").open("w", encoding="utf-8")
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, text=True, encoding="utf-8", bufsize=1)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)

    def request(self, message, timeout=900):
        self.process.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        if not self.selector.select(timeout):
            raise RuntimeError("C++评测超时，请查看 backend.log")
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("C++评测进程退出，请查看 backend.log")
        response = json.loads(line)
        if not response.get("ok"):
            raise RuntimeError(response.get("error", "C++评测失败"))
        return response

    def close(self):
        try:
            if self.process.poll() is None:
                self.request({"op": "quit"}, timeout=3)
                self.process.wait(timeout=5)
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired):
            self.process.kill()
            self.process.wait()
        finally:
            self.selector.close()
            self.process.stdin.close()
            self.process.stdout.close()
            self.log.close()


def build(build_dir, log_dir, jobs):
    print("[1/4] 构建项目C++检索器、MCP服务端与天气/日志插件……", flush=True)
    Path(log_dir).mkdir(parents=True, exist_ok=True)
    commands = [
        ["cmake", "-S", str(HERE), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release"],
        ["cmake", "--build", str(build_dir), "--target", "rag_eval_backend", "mcp_server", "weather", "log_analyzer", "-j", str(jobs)],
    ]
    with (Path(log_dir) / "build.log").open("w", encoding="utf-8") as log:
        for command in commands:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f"构建失败，详情见 {Path(log_dir) / 'build.log'}")
    plugins = build_dir / "eval_plugins"
    plugins.mkdir(exist_ok=True)
    for name, subdir in (("weather", "weather"), ("log_analyzer", "log-analyzer")):
        shutil.copy2(build_dir / "mcp_server" / "plugins" / subdir / f"lib{name}.so", plugins)
    return build_dir / "rag_eval_backend", build_dir / "mcp_server" / "mcp_server", plugins


def ask_model(key, model, url, question, tools, timeout):
    payload = {"model": model, "temperature": 0, "tool_choice": "auto",
               "messages": [
                   {"role": "system", "content": "Choose the appropriate provided tools when a request needs external data or file operations. "
                    "Generate tool calls with the parameters stated by the user. Multiple tools may be called. "
                    "For greetings or explanations that need no external data, answer directly without calling tools. "
                    "Do not call unrelated tools or invent external results."},
                   {"role": "user", "content": question}],
               "tools": [{"type": "function", "function": tool} for tool in tools]}
    request = urllib.request.Request(url, data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
                                     headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"})
    start = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = json.load(response)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"模型接口HTTP {error.code}；未输出接口错误正文") from None
    except (urllib.error.URLError, TimeoutError):
        raise RuntimeError("模型接口网络错误或超时") from None
    elapsed = (time.perf_counter() - start) * 1000
    if not body.get("choices"):
        raise RuntimeError("模型接口未返回choices")
    usage = body.get("usage", {})
    if type(usage.get("prompt_tokens")) is not int or type(usage.get("completion_tokens")) is not int:
        raise RuntimeError("接口缺少真实token用量，拒绝以字符数代替")
    if usage["prompt_tokens"] < 0 or usage["completion_tokens"] < 0:
        raise RuntimeError("接口返回非法token用量")
    return body["choices"][0]["message"], usage, elapsed


def evaluate(args, executable, server, plugins, directory, log_dir):
    suite = cases()
    if args.quick:
        chosen = {"weather-2", "tail-2", "filter-2", "summary-2", "gh-search-1", "slack-post-1",
                  "fs-list-1", "web-search-1", "multi-3", "multi-7", "no-tool-2", "no-tool-4"}
        suite = [c for c in suite if c["split"] == "dev" or c["id"] in chosen]
    data = {"status": "进行中", "evidence": "fixture" if getattr(args, "fixture", False) else "live", "config": {
        "model": args.model, "embedding_model": args.embedding_model,
        "threshold": args.threshold, "repeats": args.repeats, "quick": args.quick,
        "chat_url": args.chat_url, "embedding_url": args.embedding_url},
        "cases": suite, "stages": [], "records": [], "started_at": dt.datetime.now(dt.timezone.utc).isoformat()}
    bridge = Bridge(executable, log_dir)
    try:
        print("[2/4] 通过真实MCP连接发现工具，生成带干扰项的扩展目录……", flush=True)
        real = bridge.request({"op": "discover", "server": str(server), "plugins": str(plugins), "logs": str(log_dir)})["tools"]
        names = {t["name"] for t in real}
        required = {c["name"] for case in suite for c in case["expected_calls"]}
        # 标注既可能指向真实插件工具，也可能指向纯元数据的空壳工具（不执行、只评分）。
        unknown = required - names - {t["name"] for t in synthetic_tools()}
        if unknown:
            raise RuntimeError(f"标注引用了目录中不存在的工具：{sorted(unknown)}")
        data["real_tool_names"] = sorted(names)
        data["catalogs"] = []
        sizes = [50] if args.quick else args.sizes
        for tools in catalogs(real, sizes):
            size = len(tools)
            ks = sorted(set(k for k in args.k_values if 0 < k < size))
            if not ks:
                raise RuntimeError("没有小于工具总数的有效k值")
            data["catalogs"].append({"size": size, "tools": tools,
                                     "synthetic_names": [t["name"] for t in tools if t["name"] not in names]})
            print(f"\n[3/4] {size}个工具：新增索引，开发集比较k={ks}。", flush=True)
            indexed = bridge.request({"op": "index", "tools": tools,
                                      "embedding_model": args.embedding_model, "dimension": args.dimension,
                                      "embedding_url": args.embedding_url, "threshold": args.threshold,
                                      "index_path": str(Path(log_dir) / "tool_index.json")})
            retrieval, warm = {}, {}
            metrics = {}
            # 基线目录只有 4 个真实工具，跨应用题的预期工具不在其中；这类题跳过，
            # 不记成失败，也不参与该目录的检索指标。
            stage_names = {t["name"] for t in tools}
            stage_suite = [c for c in suite
                           if {call["name"] for call in c["expected_calls"]} <= stage_names]
            development = [c for c in stage_suite if c["split"] == "dev"]
            for case in development:
                retrieval[case["id"]] = bridge.request({"op": "retrieve", "query": case["question"], "k": max(ks)})
            def dev_key(k):
                values = [coverage(c, [t["name"] for t in retrieval[c["id"]]["tools"][:k]]) for c in development]
                return (mean([int(v[1]) for v in values if v[1] is not None]), mean([v[0] for v in values]), -k)
            chosen_k = max(ks, key=dev_key)
            bridge.request({"op": "reset_query_cache"})
            test = [c for c in stage_suite if c["split"] == "test"]
            for i, case in enumerate(test):
                first = bridge.request({"op": "retrieve", "query": case["question"], "k": chosen_k})
                warm[case["id"]] = bridge.request({"op": "retrieve", "query": case["question"], "k": chosen_k})
                if first["cache_misses"] != 1 or warm[case["id"]]["cache_misses"] != 0 or warm[case["id"]]["cache_hits"] != 1:
                    raise RuntimeError("冷查询/重复查询缓存状态不符合预期")
                # Extra k values share the already computed query vector. Timings
                # remain those measured at the actually chosen k.
                expanded = bridge.request({"op": "retrieve", "query": case["question"], "k": max(ks)})
                if expanded["tools"][:chosen_k] != first["tools"]:
                    raise RuntimeError("排序结果前缀与实际k检索不一致，拒绝混用候选和计时")
                first["tools"], first["scores"] = expanded["tools"], expanded["scores"]
                retrieval[case["id"]] = first
                if (i + 1) % 6 == 0:
                    print(f"  测试集检索进度：{i+1}/{len(test)}", flush=True)
            for k in ks:
                values = []
                for case in stage_suite:
                    subset = retrieval[case["id"]]["tools"][:k]
                    recall, complete = coverage(case, [t["name"] for t in subset])
                    values.append((case["split"], recall, complete, len(subset)))
                metrics[str(k)] = {
                    "dev_complete": mean([int(v[2]) for v in values if v[0] == "dev" and v[2] is not None]),
                    "dev_recall": mean([v[1] for v in values if v[0] == "dev"]),
                    "test_complete": mean([int(v[2]) for v in values if v[0] == "test" and v[2] is not None]),
                    "test_recall": mean([v[1] for v in values if v[0] == "test"]),
                    "candidates": mean([v[3] for v in values if v[0] == "test"])}
            stage = {"size": size, "chosen_k": chosen_k, "index": indexed, "retrieval_metrics": metrics,
                     "cases": len(stage_suite), "test_cases": len(test),
                     "skipped": len(suite) - len(stage_suite),
                     "retrieval_first_ms": mean([retrieval[c["id"]]["retrieval_ms"] for c in test]),
                     "retrieval_warm_ms": mean([r["retrieval_ms"] for r in warm.values()]),
                     "query_misses": sum(retrieval[c["id"]]["cache_misses"] for c in test),
                     "query_hits": sum(retrieval[c["id"]]["cache_hits"] for c in test), "queries": retrieval}
            print(f"  开发集选定k={chosen_k}；本目录 {len(stage_suite)} 题参与（跳过 {stage['skipped']} 题："
                  f"所需工具不在该目录内）；[4/4] {len(test)}题 × 两组 × {args.repeats}次真实模型对照。", flush=True)
            records = []
            for repeat in range(1, args.repeats + 1):
                for i, case in enumerate(test):
                    selected = retrieval[case["id"]]["tools"][:chosen_k]
                    for arm in (("all", "rag") if (i + repeat) % 2 else ("rag", "all")):
                        supplied = tools if arm == "all" else selected
                        timing = retrieval[case["id"]]
                        record = {"size": size, "arm": arm, "repeat": repeat, "id": case["id"],
                                  "kind": case["kind"], "question": case["question"],
                                  "expected_names": [c["name"] for c in case["expected_calls"]],
                                  "candidate_names": [t["name"] for t in supplied],
                                  "retrieval_ms": 0.0 if arm == "all" else (timing["retrieval_ms"] if repeat == 1 else warm[case["id"]]["retrieval_ms"]),
                                  "warm_retrieval_ms": 0.0 if arm == "all" else warm[case["id"]]["retrieval_ms"],
                                  "query_cache_misses": 0 if arm == "all" or repeat > 1 else timing["cache_misses"]}
                        try:
                            message, usage, model_ms = ask_model(os.environ["DASHSCOPE_API_KEY"], args.model,
                                                                args.chat_url, case["question"], supplied, args.timeout)
                            record.update(score(message, case, supplied))
                            record.update({"prompt_tokens": usage["prompt_tokens"], "completion_tokens": usage["completion_tokens"],
                                           "usage": usage, "model_ms": model_ms, "response": message})
                        except (RuntimeError, ValueError, KeyError) as error:
                            record["api_error"] = str(error)
                        records.append(record)
                        data["records"].append(record)
                    print(f"  对照进度：第{repeat}次 {i+1}/{len(test)}题", flush=True)
            stage["summary"] = {arm: aggregate([r for r in records if r["arm"] == arm]) for arm in ("all", "rag")}
            data["stages"].append(stage)
            data["status"] = "不完整：存在API失败" if any(r.get("api_error") for r in data["records"]) else "完成"
            report(data, directory)
        console(data, directory)
        LAST_RUN.clear()
        LAST_RUN.update(data)
        return 1 if "不完整" in data["status"] else 0
    except BaseException:
        data["status"] = "失败：评测中断，已有数据仅供排查"
        LAST_RUN.clear()
        LAST_RUN.update(data)
        if data["stages"]:
            report(data, directory)
        raise
    finally:
        bridge.close()


def main():
    parser = argparse.ArgumentParser(description="C++ RAG真实效果对照；所有文件位于tests/rag_effectiveness")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--self-test", action="store_true", help="离线自检，不调用真实API，不产生效果结论")
    mode.add_argument("--live", action="store_true", help="调用真实Embedding/模型API进行对照")
    parser.add_argument("--quick", action="store_true", help="小规模在线试跑：12个测试问题，基线+50工具")
    parser.add_argument("--model", default="qwen-plus")
    parser.add_argument("--embedding-model", default="text-embedding-v2")
    parser.add_argument("--dimension", type=int, default=1536)
    parser.add_argument("--chat-url", default=DEFAULT_CHAT)
    parser.add_argument("--embedding-url", default=DEFAULT_EMBEDDING)
    parser.add_argument("--threshold", type=float, default=0.3)
    parser.add_argument("--k-values", type=lambda s: [int(v) for v in s.split(",")], default=[1, 3, 5])
    parser.add_argument("--sizes", type=lambda s: [int(v) for v in s.split(",")], default=[50, 100, 300])
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "rag_effectiveness")
    parser.add_argument("--output", type=Path, default=HERE / "results")
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("项目使用POSIX进程通信，请在Linux或WSL中运行")
    if args.repeats < 1 or args.timeout < 1 or args.jobs < 1 or args.dimension < 1 or not 0 <= args.threshold <= 1:
        parser.error("次数、超时、线程数、维度必须为正；阈值为0..1")
    if not args.k_values or any(k < 1 for k in args.k_values) or any(n < 1 for n in args.sizes):
        parser.error("k和目录规模必须为正")
    if args.live and not os.environ.get("DASHSCOPE_API_KEY", "").strip():
        print("[未运行] 请先设置DASHSCOPE_API_KEY；缺少密钥不是测试通过。", file=sys.stderr)
        return 77
    identifier = dt.datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
    directory = args.output.resolve() / (("self-test-" if args.self_test else "live-") + identifier)
    log_dir = args.build_dir.resolve() / "logs" / directory.name
    try:
        executable, server, plugins = build(args.build_dir.resolve(), log_dir, args.jobs)
        if args.self_test:
            env = dict(os.environ, RAG_EVAL_BACKEND=str(executable), RAG_EVAL_SERVER=str(server), RAG_EVAL_PLUGINS=str(plugins))
            env["NO_PROXY"] = "127.0.0.1,localhost"
            result = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(HERE), "-p", "test_*.py", "-v"], env=env)
            if result.returncode == 0:
                print("\n[自检通过] 真实C++检索器 + 本地模拟Embedding/模型接口；不是RAG效果评测结果。")
            return result.returncode
        directory.mkdir(parents=True, exist_ok=True)
        return evaluate(args, executable, server, plugins, directory, log_dir)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        if not args.self_test:
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "report.md").write_text(
                f"# RAG 工具筛选对照\n\n状态：失败，未完成评测\n\n原因：{error}\n\n日志：{log_dir}\n",
                encoding="utf-8")
        print(f"[失败] {error}\n日志目录：{log_dir}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
