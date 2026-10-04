"""Scoring and readable reports; metrics use measured API usage, never byte counts."""
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
            "completion_tokens": mean([r["completion_tokens"] for r in valid]),
            "model_ms": mean([r["model_ms"] for r in valid]),
            "total_ms": mean([r["model_ms"] + r["retrieval_ms"] for r in valid]),
            "warm_total_ms": mean([r["model_ms"] + r["warm_retrieval_ms"] for r in valid]),
            "no_tool_accuracy": mean([int(r["correct"]) for r in valid if r["kind"] == "no_tool"]),
            "multi_accuracy": mean([int(r["correct"]) for r in valid if r["kind"] == "multi"])}


def percent(value):
    return "未测得" if value is None else f"{value * 100:.1f}%"


def number(value, suffix=""):
    return "未测得" if value is None else f"{value:.1f}{suffix}"


def report(data, directory):
    directory = Path(directory)
    lines = ["# RAG 工具筛选效果报告", "", f"**状态：{data['status']}**", "",
             "## 1. 测试了什么", "",
             f"- 模型：`{data['config']['model']}`；Embedding：`{data['config']['embedding_model']}`。",
             f"- {len(data['cases'])} 个标注问题：{sum(c['split'] == 'dev' for c in data['cases'])} 个开发问题用于选 k，{sum(c['split'] == 'test' for c in data['cases'])} 个测试问题用于最终对照。重复次数：{data['config']['repeats']}。",
             "- 基线目录通过真实 MCP 服务端发现天气、日志两个插件的工具；50/100 工具目录增加模拟工具定义。",
             "- 本报告评测检索及模型首轮工具选择、参数生成；不执行业务API，不代表最终任务成功率。",
             "- 模拟项是功能不同的工具定义，包含天气/日志近似功能与其他业务工具；不是已实现的插件。",
             "- k 只在开发集上选择；测试集不参与调参。温度固定为0，两组请求交替先后。",
             "", "## 2. 检索有没有漏掉工具", "",
             "完整命中率：问题所需的所有工具都在候选中。平均召回率：每题找回的必要工具占比。无需工具的问题不计入这两项。",
             "", "|目录工具数|k|开发集完整命中率|测试集完整命中率|测试集平均召回率|平均候选数|",
             "|---:|---:|---:|---:|---:|---:|"]
    if data.get("evidence") == "fixture":
        lines[2:2] = ["**仅模拟接口自检：下面的准确率、token与时间不能作为真实RAG效果结论。**", ""]
    for stage in data["stages"]:
        for k, metrics in stage["retrieval_metrics"].items():
            mark = "（选用）" if int(k) == stage["chosen_k"] else ""
            lines.append(f"|{stage['size']}|{k}{mark}|{percent(metrics['dev_complete'])}|"
                         f"{percent(metrics['test_complete'])}|{percent(metrics['test_recall'])}|{number(metrics['candidates'])}|")
    lines += ["", "## 3. 全量工具与 RAG 对照", "",
              "调用正确率要求工具集合、参数类型和题目要求的参数值均正确；工具选择正确率只判断工具集合。",
              "输入 token 来自真实接口 usage，不用JSON字符数代替。总耗时是检索耗时加模型首轮请求耗时，不含业务执行。",
              "", "|工具数|方式|有效/计划请求|工具选择正确率|调用正确率|平均输入token|平均模型耗时|平均总耗时|",
              "|---:|---|---:|---:|---:|---:|---:|---:|"]
    for stage in data["stages"]:
        for arm in ("all", "rag"):
            row = stage["summary"][arm]
            label = "全量" if arm == "all" else f"RAG k={stage['chosen_k']}"
            lines.append(f"|{stage['size']}|{label}|{row['valid']}/{row['requests']}|{percent(row['selection_accuracy'])}|"
                         f"{percent(row['accuracy'])}|{number(row['prompt_tokens'])}|{number(row['model_ms'], 'ms')}|{number(row['total_ms'], 'ms')}|")
    lines += ["", "## 4. 缓存、额外开销与收益", ""]
    for stage in data["stages"]:
        a, b = stage["summary"]["all"], stage["summary"]["rag"]
        savings = None if not a["prompt_tokens"] or b["prompt_tokens"] is None else 1 - b["prompt_tokens"] / a["prompt_tokens"]
        difference = None if a["accuracy"] is None or b["accuracy"] is None else (b["accuracy"] - a["accuracy"]) * 100
        lines += [f"### {stage['size']} 个工具", "",
                  f"- 输入 token 减少比例：**{percent(savings)}**；调用正确率变化：**{number(difference, ' 个百分点')}**。",
                  f"- 索引扩展耗时：{number(stage['index']['build_ms'], 'ms')}；新增工具文本缓存未命中：{stage['index']['cache_misses']} 次。",
                  f"- 测试集首轮查询：缓存未命中 {stage['query_misses']} 次、命中 {stage['query_hits']} 次；平均检索耗时 {number(stage['retrieval_first_ms'], 'ms')}。",
                  f"- 同一问题再次检索：平均 {number(stage['retrieval_warm_ms'], 'ms')}；RAG缓存命中时的检索+模型总耗时 {number(b['warm_total_ms'], 'ms')}。",
                  f"- 无需工具问题正确率：全量 {percent(a['no_tool_accuracy'])} / RAG {percent(b['no_tool_accuracy'])}。",
                  f"- 多工具问题调用正确率：全量 {percent(a['multi_accuracy'])} / RAG {percent(b['multi_accuracy'])}。", ""]
    lines += ["每个目录选定k后，重新加载已有工具向量并清空查询缓存；测试集首轮检索按选定k实测冷查询，再立即重复查询测缓存命中。重复模型测试从第二轮起按缓存命中耗时计算。",
              "索引构建是一次性成本，未加入每题总耗时。Embedding缓存未命中次数不是厂商计费token，也不等于包含重试的精确HTTP请求数；本报告不宣称货币成本一定下降。",
              "接口如返回缓存token信息，会保存在原始记录中；模型侧自动缓存与网络波动仍可能影响耗时。", "",
              "## 5. 怎么判断有没有效", "",
              "1. 先看检索完整命中率：漏掉必要工具，模型通常无法完成该任务。",
              "2. 再看调用正确率：候选减少之后，是否保持或改善了正确调用。",
              "3. 最后看输入token和包含检索的总耗时：token减少不等于延迟或总成本一定减少。",
              "4. 模拟目录结果只说明这些测试问题及工具定义下的表现；少量样本不能证明生产场景的普遍提升。",
              "5. 若有API失败或评测中断，状态为不完整/失败；正确率按有效请求计算，不能直接作完整的两组结论。", "",
              "## 6. 失败案例", ""]
    failures = [r for r in data["records"] if r.get("api_error") or not r.get("correct")]
    if not failures:
        lines.append("本次有效测试请求未发现失败；样本规模仍需结合前面的限制理解。")
    for r in failures[:20]:
        lines += [f"### {r['size']}工具 / {r['arm']} / {r['id']} / 第{r['repeat']}次", "",
                  f"- 问题：{r['question']}", f"- 预期工具：{r['expected_names']}",
                  f"- 候选工具：{r['candidate_names']}", f"- 实际调用：{r.get('called', [])}",
                  f"- 原因：{r.get('api_error') or '；'.join(r.get('reasons', []))}", ""]
    lines += [f"共 {len(failures)} 条失败/异常记录；完整内容见 `details.jsonl`。", ""]
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "report.md").write_text("\n".join(lines), encoding="utf-8")
    (directory / "summary.json").write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    with (directory / "details.jsonl").open("w", encoding="utf-8") as f:
        for row in data["records"]:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")


def console(data, directory):
    title = "模拟接口自检（不是效果数据）" if data.get("evidence") == "fixture" else "真实Embedding + 模型API"
    print(f"\n========== RAG效果对照：{title} ==========")
    print("评测范围：检索、工具选择、参数生成；不含业务API执行。")
    for stage in data["stages"]:
        print(f"\n[{stage['size']}个工具，开发集选定 k={stage['chosen_k']}]")
        for arm in ("all", "rag"):
            row = stage["summary"][arm]
            label = "全量工具" if arm == "all" else "RAG筛选"
            print(f"  {label}：调用正确率 {percent(row['accuracy'])}，输入token {number(row['prompt_tokens'])}，"
                  f"检索+模型 {number(row['total_ms'], 'ms')}，有效请求 {row['valid']}/{row['requests']}")
        metric = stage["retrieval_metrics"][str(stage["chosen_k"])]
        print(f"  测试集候选完整命中率：{percent(metric['test_complete'])}")
    print(f"\n状态：{data['status']}\n详细中文报告：{Path(directory) / 'report.md'}")
