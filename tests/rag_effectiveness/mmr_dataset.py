"""Independent MMR stress cases; labels never enter retrieval or embedding requests.

The catalog deliberately contains clusters of related, distinct capabilities.
Real embeddings decide whether MMR helps; no score or expected winner is preset.
"""
from itertools import zip_longest

from dataset import definition, integer, synthetic_tools, text


SCENARIOS = {
    "cross_domain": "跨领域多需求",
    "single": "单工具对照",
    "similar_required": "相似工具都需要",
}


def cases():
    result = []

    def add(identifier, scenario, question, *names):
        result.append({"id": identifier, "scenario": scenario, "question": question,
                       "kind": "single" if len(names) == 1 else "multi",
                       "expected_calls": [{"name": name, "arguments": {}} for name in names]})

    weather = "查北京现在的天气综合报告，包括温度、湿度，纬度39.9，经度116.4"
    tail = "读取 /tmp/rag-eval/app.log 最后20行原文"
    mail = "用 Gmail 给 ops@example.com 发邮件，主题为周报，正文为本周运行正常"
    calendar = "创建日程周会，时间为2026-10-29T14:00:00到2026-10-29T15:00:00"
    directory = "列出 /tmp/rag-eval 目录的文件名"
    search = "搜索公开网页，关键词为 MCP 工具检索评测"

    # Related tools compete for a limited final budget; coverage is not guaranteed.
    add("crowded-1", "cross_domain", weather + "；另外，" + tail + "。", "get_weather", "tail_log")
    add("crowded-2", "cross_domain", mail + "；另外，" + directory + "。", "gmail_send_email", "fs_list_directory")
    add("crowded-3", "cross_domain", calendar + "；另外，" + search + "。", "gcal_create_event", "web_search")
    add("crowded-4", "cross_domain", weather + "；" + tail + "；" + mail + "。",
        "get_weather", "tail_log", "gmail_send_email")
    add("crowded-5", "cross_domain", mail + "；" + directory + "；" + calendar + "。",
        "gmail_send_email", "fs_list_directory", "gcal_create_event")
    add("crowded-6", "cross_domain", "给 GitHub 仓库 acme/backend 建 issue，标题为登录超时；" + weather + "；" + search + "。",
        "github_create_issue", "get_weather", "web_search")

    for i, (question, name) in enumerate(((weather, "get_weather"), (tail, "tail_log"),
                                         (mail, "gmail_send_email"), (calendar, "gcal_create_event"),
                                         (directory, "fs_list_directory"), (search, "web_search")), 1):
        add(f"single-control-{i}", "single", question + "。", name)

    # Deliberate negative controls: semantic similarity does not mean redundancy.
    add("similar-1", "similar_required", tail + "，并从同一日志最近200行筛出含字面关键词 timeout 的原文。",
        "tail_log", "analyze_log")
    add("similar-2", "similar_required", "从 /tmp/rag-eval/app.log 最近200行筛出包含 ERROR 的原文，"
        "同时另做最近500行 WARN 和 ERROR 的错误摘要。", "analyze_log", "summarize_errors")
    add("similar-3", "similar_required", "读取 /tmp/rag-eval/app.log 全部原文，同时统计这个文件的总行数。",
        "read_entire_log", "count_log_lines")
    add("similar-4", "similar_required", "在 Gmail 搜索关键词发布通知，同时给 ops@example.com 发邮件，"
        "主题为上线确认，正文为已检查。", "gmail_search_messages", "gmail_send_email")
    add("similar-5", "similar_required", search + "，同时抓取 https://example.com/blog/rag 的正文。",
        "web_search", "web_fetch_page")
    add("similar-6", "similar_required", "在 Notion 搜索标题含发布计划的页面，"
        "同时向已知页面 page-7f21 追加文字：评审完成。", "notion_search_pages", "notion_append_blocks")
    return result


def nearby_tools():
    """Narrow operations with different semantics, rather than copies of a target."""
    weather, email, log = [], [], []
    metrics = ("temperature", "humidity", "wind_speed", "wind_direction", "pressure", "visibility",
               "cloud_cover", "rainfall", "snowfall", "uv_index", "dew_point", "feels_like_temperature")
    for period, description in (("current", "current observation"), ("hourly", "hourly forecast"),
                                ("daily", "daily forecast"), ("historical", "historical observation")):
        for metric in metrics:
            weather.append(definition(f"weather_{period}_{metric}",
                f"Read only {metric.replace('_', ' ')} for a city's {description}; returns one weather metric.",
                {"city": text("City"), "date": text("Observation or forecast date")}, ["city"]))
    for folder in ("inbox", "sent", "drafts", "spam", "trash", "starred", "important", "archive"):
        for action in ("list", "search", "count"):
            email.append(definition(f"mail_{action}_{folder}",
                f"{action.title()} email messages only in the {folder} folder; does not send an email.",
                {"query": text("Email search keywords")}, []))
    for level in ("trace", "debug", "info", "warn", "error", "fatal"):
        for action, description in (("first", "Read the first N matching lines"),
                                    ("last", "Read the last N matching lines"),
                                    ("count", "Count matching lines without returning text")):
            log.append(definition(f"log_{action}_{level}",
                f"{description} of a log file, filtered exclusively to severity {level.upper()}.",
                {"path": text("Log file path"), "lines": integer("Maximum lines", 1)}, ["path"]))
    return [tool for group in zip_longest(weather, email, log) for tool in group if tool is not None]


def catalogs(real_tools, sizes):
    real_tools = sorted(real_tools, key=lambda tool: tool["name"])
    existing = {tool["name"]: tool for tool in real_tools + synthetic_tools()}
    required = {call["name"] for case in cases() for call in case["expected_calls"]}
    # Every size contains every target, with unchanged definitions from the main suite.
    pool = real_tools + [existing[name] for name in sorted(required - {t["name"] for t in real_tools})]
    minimum = len(pool)
    seen = {tool["name"] for tool in pool}
    for tool in nearby_tools() + synthetic_tools():
        if tool["name"] not in seen:
            seen.add(tool["name"])
            pool.append(tool)
    result = []
    for size in sorted(set(sizes)):
        if size < minimum or size > len(pool):
            raise ValueError(f"MMR专项目录规模 {size} 超出可用范围 {minimum}..{len(pool)}")
        result.append(pool[:size])
    return result
