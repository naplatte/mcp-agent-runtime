"""Scoring and a single concise Chinese report; metrics use measured API usage, never byte counts."""
import json
import math
import statistics
from collections import Counter
from pathlib import Path

from dataset import CITY_ALIASES


def schema_errors(value, schema, path="arguments"):
    errors = []
    expected_type = schema.get("type")
    types = {"object": lambda v: isinstance(v, dict), "array": lambda v: isinstance(v, list),
             "string": lambda v: isinstance(v, str), "integer": lambda v: type(v) is int,
             "number": lambda v: type(v) in (int, float) and math.isfinite(v),
             "boolean": lambda v: type(v) is bool, "null": lambda v: v is None}
    if expected_type and not types.get(expected_type, lambda v: False)(value):
        return [f"{path} 类型应为 {expected_type}"]
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path} 不在允许值中")
    if isinstance(value, dict):
        properties = schema.get("properties", {})
        errors += [f"{path} 缺少 {key}" for key in schema.get("required", []) if key not in value]
        if schema.get("additionalProperties") is False:
            errors += [f"{path} 多出 {key}" for key in value if key not in properties]
        for key in value.keys() & properties.keys():
            errors += schema_errors(value[key], properties[key], f"{path}.{key}")
    elif isinstance(value, list):
        for i, item in enumerate(value):
            errors += schema_errors(item, schema.get("items", {}), f"{path}[{i}]")
    elif type(value) in (int, float):
        if not math.isfinite(value):
            errors.append(f"{path} 必须是有限数字")
        for key, compare in (("minimum", lambda a, b: a >= b), ("maximum", lambda a, b: a <= b)):
            if key in schema and not compare(value, schema[key]):
                errors.append(f"{path} 不满足 {key}={schema[key]}")
    return errors


def equivalent(key, actual, expected):
    if key in ("latitude", "longitude"):
        try:
            return abs(float(actual) - float(expected)) < 1e-6
        except (TypeError, ValueError):
            return False
    if key == "city" and isinstance(actual, str):
        return actual.casefold() in [v.casefold() for v in [expected] + CITY_ALIASES.get(expected, [])]
    if key == "keyword" and isinstance(actual, str) and isinstance(expected, str):
        return actual.casefold() == expected.casefold()  # log-analyzer matches case-insensitively.
    return actual == expected


def score(message, case, supplied):
    definitions = {t["name"]: t for t in supplied}
    expected = {c["name"]: c["arguments"] for c in case["expected_calls"]}
    calls = message.get("tool_calls") or []
    if not isinstance(calls, list):
        return {"selection_correct": False, "arguments_correct": False, "correct": False,
                "called": [], "reasons": ["tool_calls 不是数组"]}
    parsed, reasons = [], []
    for call in calls:
        try:
            function = call["function"]
            name = function["name"]
            arguments = json.loads(function["arguments"])
            if name not in definitions:
                reasons.append(f"调用了候选集合外的工具 {name}")
            else:
                reasons.extend(schema_errors(arguments, definitions[name]["parameters"]))
            parsed.append((name, arguments))
        except (KeyError, TypeError, ValueError):
            reasons.append("工具调用或参数JSON格式错误")
    names = [name for name, _ in parsed]
    selection = Counter(names) == Counter(expected.keys()) and len(parsed) == len(calls)
    if not selection:
        reasons.append(f"工具不匹配：预期 {list(expected)}，实际 {names}")
    if not expected and not calls and not str(message.get("content") or "").strip():
        reasons.append("无需工具的问题没有获得直接回答")
    for name, arguments in parsed:
        if name not in expected or not isinstance(arguments, dict):
            continue
        props = definitions.get(name, {}).get("parameters", {}).get("properties", {})
        for key, value in expected[name].items():
            actual = arguments.get(key, props.get(key, {}).get("default"))
            if not equivalent(key, actual, value):
                reasons.append(f"{name}.{key} 参数值与题目要求不符")
    return {"selection_correct": selection, "arguments_correct": selection and not reasons,
            "correct": selection and not reasons, "called": names, "reasons": reasons}


def coverage(case, names):
    expected = {c["name"] for c in case["expected_calls"]}
    if not expected:
        return None, None
    found = len(expected & set(names))
    return found / len(expected), found == len(expected)


def mean(values):
    values = [v for v in values if v is not None]
    return statistics.mean(values) if values else None


def aggregate(records):
    valid = [r for r in records if not r.get("api_error")]
    return {"requests": len(records), "valid": len(valid), "api_errors": len(records) - len(valid),
            "selection_accuracy": mean([int(r["selection_correct"]) for r in valid]),
            "accuracy": mean([int(r["correct"]) for r in valid]),
            "prompt_tokens": mean([r["prompt_tokens"] for r in valid]),
            "model_ms": mean([r["model_ms"] for r in valid]),
            "total_ms": mean([r["model_ms"] + r["retrieval_ms"] for r in valid]),
            "no_tool_accuracy": mean([int(r["correct"]) for r in valid if r["kind"] == "no_tool"]),
            "multi_accuracy": mean([int(r["correct"]) for r in valid if r["kind"] == "multi"])}


def percent(value):
    return "未测得" if value is None else f"{value * 100:.1f}%"


def number(value, suffix=""):
    return "未测得" if value is None else f"{value:.0f}{suffix}"


def report(data, directory):
    kinds = Counter(c["kind"] for c in data["cases"])
    dev = sum(c["split"] == "dev" for c in data["cases"])
    lines = ["# RAG 工具筛选对照", "",
             f"状态：{data['status']}"]
    if data.get("evidence") == "fixture":
        lines.append("**模拟接口自检：以下准确率、token 与时间不能作为真实 RAG 效果结论。**")
    lines += [f"模型：{data['config']['model']}｜Embedding：{data['config']['embedding_model']}"
              f"｜重复 {data['config']['repeats']} 次｜相似度阈值 {data['config']['threshold']}", "",
             "## 1. 题目与目录", "",
             f"{len(data['cases'])} 题（开发 {dev} / 测试 {len(data['cases']) - dev}）："
             f"{kinds['single']} 单工具、{kinds['multi']} 多工具、{kinds['no_tool']} 无需工具。"]

    sizes = [str(s["size"]) for s in data["stages"]]
    skipped = [f"{s['size']} 工具目录：{s.get('cases', 0)} 题参与" +
               (f"，跳过 {s['skipped']} 题（所需工具不在该目录内）" if s.get("skipped") else "") for s in data["stages"]]
    lines += [f"工具目录：{' / '.join(sizes)}。"] + [f"- {s}" for s in skipped]

    lines += ["", "## 2. 检索有没有漏工具（测试集）", "",
              "| 目录 | 选用 k | 完整命中率 | 平均召回率 |", "|---:|---:|---:|---:|"]
    for stage in data["stages"]:
        metric = stage["retrieval_metrics"].get(str(stage["chosen_k"]), {})
        lines.append(f"| {stage['size']} | {stage['chosen_k']} | {percent(metric.get('test_complete'))} "
                     f"| {percent(metric.get('test_recall'))} |")

    lines += ["", "## 3. 全量工具 vs RAG 筛选", "",
              "| 目录 | 方式 | 调用正确率 | 工具选择正确率 | 平均输入 token |",
              "|---:|---|---:|---:|---:|"]
    for stage in data["stages"]:
        for arm, label in (("all", "全量"), ("rag", f"RAG k={stage['chosen_k']}")):
            row = stage["summary"][arm]
            lines.append(f"| {stage['size']} | {label} | {percent(row['accuracy'])} "
                         f"| {percent(row['selection_accuracy'])} | {number(row['prompt_tokens'])} |")

    lines += ["", "## 4. 可以直接说的结论", ""]
    for stage in data["stages"]:
        base, rag = stage["summary"]["all"], stage["summary"]["rag"]
        savings = None if not base["prompt_tokens"] or rag["prompt_tokens"] is None \
            else 1 - rag["prompt_tokens"] / base["prompt_tokens"]
        delta = None if base["accuracy"] is None or rag["accuracy"] is None \
            else (rag["accuracy"] - base["accuracy"]) * 100
        lines.append(f"- {stage['size']} 个工具：输入 token 降 {percent(savings)}，"
                     f"调用正确率 {percent(base['accuracy'])} → {percent(rag['accuracy'])}"
                     f"（{'+' if (delta or 0) >= 0 else ''}{number(delta)} 个百分点）。")
    if data["stages"]:
        last = data["stages"][-1]["summary"]
        lines.append(f"- 无需工具题正确率：全量 {percent(last['all']['no_tool_accuracy'])} / "
                     f"RAG {percent(last['rag']['no_tool_accuracy'])}；"
                     f"多工具题：全量 {percent(last['all']['multi_accuracy'])} / "
                     f"RAG {percent(last['rag']['multi_accuracy'])}。")

    failures = [r for r in data["records"] if r.get("api_error") or not r.get("correct")]
    lines += ["", "## 5. 失败案例", ""]
    if not failures:
        lines.append("本次没有失败记录。")
    for r in failures[:8]:
        reason = r.get("api_error") or "；".join(r.get("reasons", [])[:2])
        lines.append(f"- [{r['size']}/{r['arm']}/{r['id']}] 预期 {r['expected_names']}，"
                     f"实际 {r.get('called', [])} —— {reason}")
    if len(failures) > 8:
        lines.append(f"- 其余 {len(failures) - 8} 条略。")

    lines += ["", "## 6. 局限", "",
              "- 除 4 个真实工具外，其余为只有元数据的模拟定义，不执行真实业务。",
              f"- 单模型、单 Embedding、重复 {data['config']['repeats']} 次；结论只对这组题成立，不是生产准确率。",
              "- 全量组与 RAG 组使用同一批工具定义与问题，比较只在同一目录内成立。", ""]

    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "report.md").write_text("\n".join(lines), encoding="utf-8")


def console(data, directory):
    print(f"\n========== RAG 工具筛选对照 ==========\n状态：{data['status']}")
    for stage in data["stages"]:
        base, rag = stage["summary"]["all"], stage["summary"]["rag"]
        metric = stage["retrieval_metrics"].get(str(stage["chosen_k"]), {})
        print(f"\n[{stage['size']} 个工具，k={stage['chosen_k']}，{stage.get('cases', 0)} 题]")
        print(f"  检索完整命中率 {percent(metric.get('test_complete'))}，平均召回 {percent(metric.get('test_recall'))}")
        print(f"  全量：调用正确率 {percent(base['accuracy'])}，token {number(base['prompt_tokens'])}")
        print(f"  RAG ：调用正确率 {percent(rag['accuracy'])}，token {number(rag['prompt_tokens'])}")
    print(f"\n报告：{Path(directory) / 'report.md'}")
