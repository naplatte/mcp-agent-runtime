"""Tool-name-only scoring and a compact Chinese report; token usage comes from the API."""
import statistics
from collections import Counter
from pathlib import Path


def score(message, case, supplied):
    """只评工具名称；参数缺失、值错或参数 JSON 无法解析均不影响这个指标。"""
    expected = [call["name"] for call in case["expected_calls"]]
    available = {tool["name"] for tool in supplied}
    calls = message.get("tool_calls")
    calls = [] if calls is None else calls
    names, reasons = [], []
    if not isinstance(calls, list):
        reasons.append("tool_calls 不是数组")
    else:
        for call in calls:
            try:
                name = call["function"]["name"]
                if not isinstance(name, str) or not name:
                    raise ValueError("工具名无效")
                names.append(name)
                if name not in available:
                    reasons.append(f"调用了候选集合外的工具 {name}")
            except (KeyError, TypeError, ValueError):
                reasons.append("工具调用缺少有效名称")
    if Counter(names) != Counter(expected):
        reasons.append(f"工具不匹配：预期 {expected}，实际 {names}")
    correct = not reasons
    return {"selection_correct": correct, "correct": correct, "called": names, "reasons": reasons}


def coverage(case, names):
    expected = {call["name"] for call in case["expected_calls"]}
    if not expected:
        return None, None
    found = len(expected & set(names))
    return found / len(expected), found == len(expected)


def mean(values):
    values = [value for value in values if value is not None]
    return statistics.mean(values) if values else None


def aggregate(records):
    valid = [record for record in records if not record.get("api_error")]
    hit = [record for record in valid if record.get("retrieval_hit", True)]
    correct = sum(record["selection_correct"] for record in valid)
    accuracy = correct / len(valid) if valid else None
    return {"requests": len(records), "valid": len(valid), "api_errors": len(records) - len(valid),
            "correct_count": correct, "selection_accuracy": accuracy, "accuracy": accuracy,
            "prompt_tokens": mean([record["prompt_tokens"] for record in valid]),
            "hit_count": len(hit), "retrieval_fail": len(valid) - len(hit),
            "selection_fail": sum(not record["selection_correct"] for record in hit)}


def percent(value):
    return "未测得" if value is None else f"{value * 100:.1f}%"


def number(value):
    return "未测得" if value is None else f"{value:,.0f}"


def report(data, directory):
    config = data["config"]
    selection = config.get("selection", "similarity")
    label = {"similarity": "普通向量", "mmr": "MMR", "hybrid": "向量＋关键词倒排"}[selection]
    lines = ["# RAG 工具选择对照", "", f"状态：{data['status']}"]
    if data.get("evidence") == "fixture":
        lines.append("**模拟接口自检：以下数据不能作为真实 RAG 效果结论。**")
    lines += [f"模型：{config['model']}｜Embedding：{config['embedding_model']}｜RAG：{label}",
              f"{len(data['cases'])} 题，重复 {config['repeats']} 次。", ""]
    if config.get("retrieval_only"):
        lines += ["仅检索：没有调用对话模型，模型工具选择命中率和输入 token 未测量。", ""]
        comparison = selection in ("mmr", "hybrid")
        lines += ([f"| 工具总数 | 普通向量：工具找齐 | {label}：工具找齐 | 最终最多工具数 |",
                   "|---:|---:|---:|---:|"] if comparison else
                  ["| 工具总数 | 工具找齐 | 最终最多工具数 |", "|---:|---:|---:|"])
        for stage in data["stages"]:
            metric = stage["retrieval"]
            current = f"{percent(metric['complete'])}（{metric['hits']}/{metric['required']}）"
            if comparison:
                base = stage["baseline_retrieval"]
                previous = f"{percent(base['complete'])}（{base['hits']}/{base['required']}）"
                lines.append(f"| {stage['size']} | {previous} | {current} | {stage['k']} |")
            else:
                lines.append(f"| {stage['size']} | {current} | {stage['k']} |")
        lines += ["", "工具找齐只统计需要工具的题；多工具题必须全部找到。"]
    else:
        lines += ["命中率只看工具是否全部选对，不漏选、不多选、不重复；忽略参数错误。无需工具的题不调用工具即算对。",
                  "括号为选对题数／有效题数；平均输入 token 来自接口实际用量。", "",
                  "| 工具总数 | 不使用RAG：命中率 | 使用RAG：命中率 | 不使用RAG：平均输入token | 使用RAG：平均输入token |",
                  "|---:|---:|---:|---:|---:|"]
        for stage in data["stages"]:
            base, rag = stage["summary"]["all"], stage["summary"]["rag"]
            base_rate = f"{percent(base['selection_accuracy'])}（{base['correct_count']}/{base['valid']}）"
            rag_rate = f"{percent(rag['selection_accuracy'])}（{rag['correct_count']}/{rag['valid']}）"
            lines.append(f"| {stage['size']} | {base_rate} | {rag_rate} "
                         f"| {number(base['prompt_tokens'])} | {number(rag['prompt_tokens'])} |")
        limits = sorted({stage['k'] for stage in data['stages']})
        lines += ["", "RAG每题最多提供 " + " / ".join(str(limit) for limit in limits) + " 个工具。"]
        failures = []
        for stage in data["stages"]:
            for arm, name in (("all", "不使用RAG"), ("rag", "使用RAG")):
                row = stage["summary"][arm]
                if row["api_errors"]:
                    failures.append(f"{stage['size']}工具／{name}：有效{row['valid']}/{row['requests']}，接口失败{row['api_errors']}次")
        if failures:
            lines += ["", "接口失败未计入命中率和平均输入量，结果不完整："] + [f"- {failure}" for failure in failures]
    lines += ["", "评测只检查工具调用，不执行模拟工具的真实业务。", ""]
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "report.md").write_text("\n".join(lines), encoding="utf-8")


def console(data, directory):
    print("\n" + (Path(directory) / "report.md").read_text(encoding="utf-8"))
    print(f"报告：{Path(directory) / 'report.md'}")
