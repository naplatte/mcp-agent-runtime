"""Offline, meaningful regression checks. All HTTP is on 127.0.0.1."""
import contextlib
from collections import Counter
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import types
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

from dataset import cases, catalogs, definition, text
from evaluation import aggregate, coverage, score
from run import Bridge, evaluate, retrieval_misses
import run


def fixture_vector(value):
    tokens = [("get_weather", "天气"), ("tail_log", "最后"),
              ("analyze_log", "timeout"), ("summarize_errors", "摘要")]
    vector = [0.0] * 8
    for i, words in enumerate(tokens):
        if any(word in value for word in words):
            vector[i] = 1.0
    if not any(vector):
        vector[4] = 1.0
    return vector


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append((self.path, request))
        if self.path == "/embedding":
            if self.server.mode == "error":
                result = {"code": "fixture_error", "message": "fixture refused embedding"}
            else:
                result = {"output": {"embeddings": [
                    {"embedding": [] if self.server.mode == "empty" else fixture_vector(value), "text_index": i}
                    for i, value in enumerate(request["input"]["texts"])]}}
        elif self.path == "/chat":
            question = request["messages"][-1]["content"]
            case = next(c for c in cases() if c["question"] == question)
            calls = [{"id": f"call_{i}", "type": "function", "function": {
                "name": call["name"], "arguments": json.dumps(call["arguments"], ensure_ascii=False)}}
                     for i, call in enumerate(case["expected_calls"])]
            message = {"role": "assistant", "content": None if calls else "这是不需要工具的直接回答。", "tool_calls": calls}
            # Unit fixture values only; never presented as real token measurements.
            result = {"choices": [{"message": message}], "usage": {
                "prompt_tokens": 10 + len(request["tools"]) * 20, "completion_tokens": 5}}
        else:
            self.send_error(404)
            return
        payload = json.dumps(result).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


@contextlib.contextmanager
def local_api():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    server.mode = "ok"
    server.requests = []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server, f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class ScoringTests(unittest.TestCase):
    def test_miss_diagnostics_distinguish_filter_pool_and_selection(self):
        case = {"id": "fixture", "kind": "single", "question": "fixture",
                "expected_calls": [{"name": "target", "arguments": {}}]}
        entry = {"name": "target", "rank": 12, "score": 0.2,
                 "passes_threshold": False, "in_pool": False}
        response = {"tools": [], "ranking": [entry]}
        def reason(selection="mmr"):
            return retrieval_misses([case], {"fixture": response}, selection, 10, 30, 0.3)[0]
        self.assertEqual(reason()["reason_code"], "threshold")
        self.assertEqual(reason()["rank"], 12)
        entry.update(rank=31, score=0.6, passes_threshold=True)
        self.assertEqual(reason()["reason_code"], "pool")
        entry.update(rank=12, in_pool=True)
        self.assertEqual(reason()["reason_code"], "mmr")
        self.assertEqual(reason("similarity")["reason_code"], "top_k")
        response["tools"] = [{"name": "target"}]
        self.assertEqual(retrieval_misses([case], {"fixture": response}, "mmr", 10, 30, 0.3), [])

    def test_cases_and_catalogs_have_distinct_capabilities(self):
        suite = cases()
        self.assertEqual(len(suite), 80)
        kinds = Counter(c["kind"] for c in suite)
        self.assertEqual((kinds["single"], kinds["multi"], kinds["no_tool"]), (40, 24, 16))
        base = [definition(name, name, {}, []) for name in ("get_weather", "tail_log", "analyze_log", "summarize_errors")]
        groups = catalogs(base, [50, 100, 300])
        self.assertEqual([len(g) for g in groups], [50, 100, 300])
        self.assertEqual(groups[1][:50], groups[0])
        self.assertEqual(groups[2][:100], groups[1])
        self.assertEqual(len({t["name"] for t in groups[-1]}), 300)
        self.assertEqual(len({t["description"] for t in groups[-1]}), 300)

    def test_tool_and_argument_failures_are_scored_separately(self):
        tool = definition("get_weather", "weather", {"city": text("City"), "latitude": text("Latitude"),
                                                   "longitude": text("Longitude")}, ["city", "latitude", "longitude"])
        case = cases()[0]

        def message(arguments, name="get_weather"):
            return {"tool_calls": [{"function": {"name": name, "arguments": json.dumps(arguments)}}]}

        good = {"city": "Beijing", "latitude": "39.900", "longitude": "116.400"}
        self.assertTrue(score(message(good), case, [tool])["correct"])
        bad = dict(good, latitude=39.9)
        scored = score(message(bad), case, [tool])
        self.assertTrue(scored["selection_correct"])
        self.assertFalse(scored["correct"])
        self.assertFalse(score(message(good, "air_quality"), case, [tool])["correct"])
        duplicate = message(good)
        duplicate["tool_calls"] *= 2
        self.assertFalse(score(duplicate, case, [tool])["selection_correct"])

    def test_multi_tool_coverage_and_no_tool(self):
        case = next(c for c in cases() if c["kind"] == "multi")
        self.assertEqual(coverage(case, ["get_weather"]), (0.5, False))
        self.assertEqual(coverage(case, ["get_weather", "tail_log"]), (1.0, True))
        no_tool = next(c for c in cases() if c["kind"] == "no_tool")
        self.assertTrue(score({"content": "你好"}, no_tool, [])["correct"])
        self.assertFalse(score({"content": ""}, no_tool, [])["correct"])

    def test_api_errors_are_not_zero_tokens_or_success(self):
        result = aggregate([{"api_error": "HTTP 401"}])
        self.assertEqual(result["api_errors"], 1)
        self.assertEqual(result["valid"], 0)
        self.assertIsNone(result["accuracy"])
        self.assertIsNone(result["prompt_tokens"])


@unittest.skipUnless(os.environ.get("RAG_EVAL_BACKEND"), "Build the C++ backend using run.py --self-test")
class BackendTests(unittest.TestCase):
    def test_mmr_reuses_query_embedding_and_exposes_unfiltered_ranking(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url):
            bridge = Bridge(os.environ["RAG_EVAL_BACKEND"], directory)
            try:
                tools = [definition("get_weather", "weather", {}, []),
                         definition("tail_log", "log", {}, []),
                         definition("other", "other", {}, [])]
                bridge.request({"op": "index", "tools": tools, "fixture_key": "offline-fixture",
                                "dimension": 8, "threshold": 0.3, "embedding_url": url + "/embedding",
                                "index_path": str(Path(directory) / "index.json")})
                before = len(api.requests)
                query = {"op": "retrieve", "query": "北京天气", "k": 2, "selection": "mmr",
                         "mmr_fetch_k": 3, "mmr_lambda": 0.7, "diagnostics": True}
                mmr = bridge.request(query)
                self.assertEqual(len(api.requests), before + 1)
                self.assertEqual(mmr["cache_misses"], 1)
                self.assertEqual(len(mmr["ranking"]), 3)
                target = next(r for r in mmr["ranking"] if r["name"] == "tail_log")
                self.assertFalse(target["passes_threshold"])
                self.assertFalse(target["in_pool"])
                self.assertFalse(target["selected"])
                ordinary = bridge.request(dict(query, selection="similarity"))
                self.assertEqual(ordinary["cache_misses"], 0)
                self.assertEqual(len(api.requests), before + 1)
                self.assertEqual(mmr["ranking"], ordinary["ranking"])
                with self.assertRaisesRegex(RuntimeError, "fetch_k"):
                    bridge.request(dict(query, mmr_fetch_k=1))
                self.assertEqual(len(api.requests), before + 1)
            finally:
                bridge.close()

    def test_retrieval_only_mmr_report_never_calls_chat(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url), \
                patch.dict(os.environ, {"DASHSCOPE_API_KEY": "offline-fixture"}):
            args = types.SimpleNamespace(quick=True, repeats=1, model="fixture", embedding_model="fixture",
                                         dimension=8, threshold=0.3, sizes=[50], k=10,
                                         selection="mmr", mmr_fetch_k=30, mmr_lambda=0.7,
                                         retrieval_only=True, fixture=True,
                                         chat_url=url + "/chat", embedding_url=url + "/embedding", timeout=3)
            with contextlib.redirect_stdout(io.StringIO()):
                code = evaluate(args, Path(os.environ["RAG_EVAL_BACKEND"]), Path(os.environ["RAG_EVAL_SERVER"]),
                                Path(os.environ["RAG_EVAL_PLUGINS"]), Path(directory), Path(directory) / "logs")
            self.assertEqual(code, 0)
            self.assertFalse(any(path == "/chat" for path, _ in api.requests))
            data = run.LAST_RUN
            self.assertEqual(data["records"], [])
            stage = data["stages"][0]
            self.assertEqual(stage["query_misses"], len(data["cases"]))
            self.assertIn("baseline_retrieval", stage)
            text_report = (Path(directory) / "report.md").read_text(encoding="utf-8")
            self.assertIn("普通排序与 MMR", text_report)
            self.assertIn("正确工具为什么没进最终候选", text_report)
            self.assertIn("相似度低于阈值", text_report)
            self.assertIn("最终调用正确率和对话输入token均未测量", text_report)

    def test_actual_cpp_discovery_retrieval_cache_and_error_boundary(self):
        with tempfile.TemporaryDirectory() as directory, local_api() as (api, url):
            bridge = Bridge(os.environ["RAG_EVAL_BACKEND"], directory)
            try:
                tools = bridge.request({"op": "discover", "server": os.environ["RAG_EVAL_SERVER"],
                                        "plugins": os.environ["RAG_EVAL_PLUGINS"], "logs": directory})["tools"]
                self.assertEqual({t["name"] for t in tools}, {"get_weather", "tail_log", "analyze_log", "summarize_errors"})
                response = bridge.request({"op": "index", "tools": tools, "fixture_key": "offline-fixture",
                                           "dimension": 8, "threshold": 0.3, "embedding_url": url + "/embedding",
                                           "index_path": str(Path(directory) / "index.json")})
                self.assertEqual(response["indexed"], 4)
                first = bridge.request({"op": "retrieve", "query": "北京天气", "k": 1})
                self.assertEqual(first["tools"][0]["name"], "get_weather")
                self.assertEqual(first["cache_misses"], 1)
                before = len(api.requests)
                warm = bridge.request({"op": "retrieve", "query": "北京天气", "k": 1})
                self.assertEqual(warm["cache_hits"], 1)
                self.assertEqual(len(api.requests), before)
                bridge.request({"op": "reset_query_cache"})
                cold = bridge.request({"op": "retrieve", "query": "北京天气", "k": 1})
                self.assertEqual(cold["cache_misses"], 1)
                # k 超过目录大小时按全量返回：4 个工具、k=10 仍应拿到 4 个
                full = bridge.request({"op": "retrieve", "query": "北京天气", "k": 10})
                self.assertEqual(len(full["tools"]), 4)
                with self.assertRaisesRegex(RuntimeError, "positive"):
                    bridge.request({"op": "retrieve", "query": "北京天气", "k": 0})
            finally:
                bridge.close()

    def test_empty_and_failed_embeddings_cannot_pass_index_validation(self):
        for mode in ("empty", "error"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory, local_api() as (api, url):
                api.mode = mode
                bridge = Bridge(os.environ["RAG_EVAL_BACKEND"], directory)
                try:
                    tools = [definition("sample", "fixture", {}, [])]
                    with self.assertRaisesRegex(RuntimeError, "embedding|Incomplete"):
                        bridge.request({"op": "index", "tools": tools, "fixture_key": "offline-fixture",
                                        "dimension": 8, "embedding_url": url + "/embedding",
                                        "index_path": str(Path(directory) / "index.json")})
                finally:
                    bridge.close()

    def test_complete_driver_reports_are_explicitly_fixture_only(self):
        self.check_complete_driver("similarity")

    def test_complete_mmr_driver_reports_are_explicitly_fixture_only(self):
        self.check_complete_driver("mmr")

    def check_complete_driver(self, selection):
        with tempfile.TemporaryDirectory() as directory, local_api() as (_, url), patch.dict(os.environ, {"DASHSCOPE_API_KEY": "offline-fixture"}):
            args = types.SimpleNamespace(quick=False, repeats=2, model="fixture", embedding_model="fixture",
                                         dimension=8, threshold=0.3, sizes=[50, 100, 300], k=10,
                                         selection=selection, mmr_fetch_k=30, mmr_lambda=0.7,
                                         chat_url=url + "/chat", embedding_url=url + "/embedding", timeout=3, fixture=True)
            with contextlib.redirect_stdout(io.StringIO()):
                code = evaluate(args, Path(os.environ["RAG_EVAL_BACKEND"]), Path(os.environ["RAG_EVAL_SERVER"]),
                                Path(os.environ["RAG_EVAL_PLUGINS"]), Path(directory), Path(directory) / "logs")
            self.assertEqual(code, 0)
            data = run.LAST_RUN
            self.assertEqual(data["evidence"], "fixture")
            self.assertEqual([s["size"] for s in data["stages"]], [50, 100, 300])
            expected = sum(s["test_cases"] * 2 * args.repeats for s in data["stages"])
            self.assertEqual(len(data["records"]), expected)
            for stage in data["stages"]:
                # k 不小于目录大小时走退化分支，不产生查询向量
                expected_misses = stage["test_cases"] if stage["k"] < stage["size"] else 0
                self.assertEqual(stage["query_misses"], expected_misses)
                self.assertEqual(stage["query_hits"], 0)
            self.assertTrue(all(r["query_cache_misses"] == 0 for r in data["records"] if r["repeat"] == 2))
            # 结果目录里只留一个中文报告
            self.assertEqual([p.name for p in Path(directory).iterdir() if p.is_file()], ["report.md"])
            text_report = (Path(directory) / "report.md").read_text()
            self.assertIn("不能作为真实 RAG 效果结论", text_report)
            self.assertIn("全量", text_report)
            self.assertIn("失败案例", text_report)


if __name__ == "__main__":
    unittest.main()
