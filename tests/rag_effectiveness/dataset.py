"""Reviewed labels + synthetic tool metadata. No external data or business API calls.

目录构成：
- 4 个真实插件工具（天气 get_weather、日志 tail_log/analyze_log/summarize_errors），
  由真实 MCP 服务端发现，可执行。
- 12 个"近似干扰"工具：贴着真实工具造的近义能力，用来测细粒度区分。
- 34 个"热门 MCP 服务"空壳工具：GitHub / Slack / Notion / 文件系统 / Postgres /
  浏览器 / Web / Sentry / Jira / Linear / Gmail / Google Calendar / Drive / Memory。
  只有元数据，没有实现；评分只比对 tool_calls，不执行工具，因此可以直接作为
  检索与工具选择的评测目标。
- 84 个按业务域批量生成的工具（28 域 × list/get/create），只作干扰项。

题目只有三类：单工具、多工具、无工具，数量比约 5:3:2。
"""


def definition(name, description, properties, required):
    return {"name": name, "description": description, "parameters": {
        "type": "object", "properties": properties, "required": required,
        "additionalProperties": False}}


def text(description):
    return {"type": "string", "description": description}


def integer(description, minimum=None):
    schema = {"type": "integer", "description": description}
    if minimum is not None:
        schema["minimum"] = minimum
    return schema


def synthetic_tools():
    # These are distinct capabilities, not repeated tools with numbered names.
    nearby = [
        definition("weather_history", "Read historical weather observations for a past date; not a current weather report.",
                   {"city": text("City"), "date": text("Past date YYYY-MM-DD")}, ["city", "date"]),
        definition("air_quality", "Read air quality pollutants PM2.5 and PM10; this does not provide weather temperature or humidity.",
                   {"city": text("City")}, ["city"]),
        definition("pollen_forecast", "Read pollen concentration and allergy risk for a city.",
                   {"city": text("City")}, ["city"]),
        definition("sunrise_sunset", "Read sunrise and sunset times for a location and date.",
                   {"latitude": text("Latitude"), "longitude": text("Longitude"), "date": text("Date")},
                   ["latitude", "longitude", "date"]),
        definition("weather_station_list", "List weather monitoring stations near a city, without fetching a weather report.",
                   {"city": text("City")}, ["city"]),
        definition("weather_alerts", "Read severe weather warning bulletins, not the ordinary weather report.",
                   {"city": text("City")}, ["city"]),
        definition("count_log_lines", "Count the number of lines in a log file; returns a number, not line contents.",
                   {"path": text("Log file path")}, ["path"]),
        definition("read_entire_log", "Read every line of a complete log file, not only its last N lines.",
                   {"path": text("Log file path")}, ["path"]),
        definition("search_log_regex", "Search log lines using a regular expression, rather than a literal keyword.",
                   {"path": text("Log file path"), "pattern": text("Regular expression")}, ["path", "pattern"]),
        definition("archive_log", "Compress a log file into an archive without analyzing its contents.",
                   {"path": text("Log file path")}, ["path"]),
        definition("configure_log_rotation", "Configure log rotation size and retention; does not read logs.",
                   {"path": text("Log file path"), "retention_days": {"type": "integer", "minimum": 1}},
                   ["path", "retention_days"]),
        definition("count_error_levels", "Return only the numeric count of each severity level in a log, without a textual summary of errors.",
                   {"path": text("Log file path")}, ["path"]),
    ]
    # 热门 MCP 服务形态的空壳工具：只有元数据，没有实现。
    popular = [
        definition("github_search_code", "Search source code inside GitHub repositories by keyword; returns file paths and snippets.",
                   {"query": text("Code keyword"), "repo": text("Repository as owner/name")}, ["query"]),
        definition("github_create_issue", "Create a new issue in a GitHub repository; title is required, body is optional.",
                   {"repo": text("Repository as owner/name"), "title": text("Issue title"), "body": text("Issue body")},
                   ["repo", "title"]),
        definition("github_list_pull_requests", "List pull requests of a GitHub repository filtered by state.",
                   {"repo": text("Repository as owner/name"), "state": text("open, closed or all")}, ["repo"]),
        definition("github_get_commit", "Read one commit's message and changed files from a GitHub repository.",
                   {"repo": text("Repository as owner/name"), "sha": text("Commit SHA")}, ["repo", "sha"]),
        definition("slack_post_message", "Post a new message to a Slack channel; this sends immediately.",
                   {"channel": text("Channel name such as #deploy"), "text": text("Message text")}, ["channel", "text"]),
        definition("slack_search_messages", "Search past Slack messages by keyword.",
                   {"query": text("Search keyword")}, ["query"]),
        definition("slack_list_channels", "List Slack channels the bot has joined; does not post messages.",
                   {"limit": integer("Maximum channels to return", 1)}, []),
        definition("notion_search_pages", "Search Notion workspace pages by title keyword.",
                   {"query": text("Title keyword")}, ["query"]),
        definition("notion_create_page", "Create a new Notion page under an existing parent page.",
                   {"parent_id": text("Parent page id"), "title": text("New page title")}, ["parent_id", "title"]),
        definition("notion_append_blocks", "Append content blocks to an existing Notion page; content is optional.",
                   {"page_id": text("Target page id"), "content": text("Text to append")}, ["page_id"]),
        definition("fs_read_file", "Read the text content of one local file from disk.",
                   {"path": text("Absolute file path")}, ["path"]),
        definition("fs_write_file", "Write text content into a local file, overwriting it.",
                   {"path": text("Absolute file path"), "content": text("Text to write")}, ["path", "content"]),
        definition("fs_list_directory", "List files and subdirectories under a local directory; does not read file contents.",
                   {"path": text("Absolute directory path")}, ["path"]),
        definition("fs_move_file", "Move or rename a local file from one path to another.",
                   {"source": text("Current path"), "target": text("Destination path")}, ["source", "target"]),
        definition("postgres_query", "Run a read-only SQL query against the Postgres database and return rows.",
                   {"sql": text("SQL statement")}, ["sql"]),
        definition("postgres_list_tables", "List table names in one Postgres schema; does not describe columns.",
                   {"schema": text("Schema name such as public")}, []),
        definition("postgres_describe_table", "Describe columns, types and indexes of one Postgres table.",
                   {"table": text("Table name")}, ["table"]),
        definition("browser_navigate", "Open a URL in a headless browser and return the page text.",
                   {"url": text("Absolute URL")}, ["url"]),
        definition("browser_screenshot", "Take a screenshot of the current browser page and save it to a file.",
                   {"path": text("Output image path")}, ["path"]),
        definition("browser_click", "Click an element matched by a CSS selector on the current browser page.",
                   {"selector": text("CSS selector")}, ["selector"]),
        definition("web_search", "Search the public web and return result titles and URLs; does not fetch page bodies.",
                   {"query": text("Search query")}, ["query"]),
        definition("web_fetch_page", "Fetch one URL and return its cleaned text content.",
                   {"url": text("Absolute URL")}, ["url"]),
        definition("sentry_list_issues", "List unresolved Sentry issues of one project; does not resolve them.",
                   {"project": text("Sentry project slug"), "limit": integer("Maximum issues to return", 1)}, ["project"]),
        definition("sentry_get_issue", "Read one Sentry issue's stack trace and tags by its issue id.",
                   {"issue_id": text("Issue id such as PROJ-1234")}, ["issue_id"]),
        definition("sentry_resolve_issue", "Mark one Sentry issue as resolved.",
                   {"issue_id": text("Issue id such as PROJ-1234")}, ["issue_id"]),
        definition("jira_create_issue", "Create a new Jira issue in a project.",
                   {"project": text("Jira project key"), "summary": text("Issue summary"),
                    "issue_type": text("Issue type such as Task or Bug")}, ["project", "summary"]),
        definition("jira_list_sprints", "List sprints of one Jira board.",
                   {"board_id": text("Jira board id")}, ["board_id"]),
        definition("linear_create_issue", "Create a new issue in a Linear team.",
                   {"team": text("Linear team name"), "title": text("Issue title")}, ["team", "title"]),
        definition("linear_list_issues", "List open issues assigned to one Linear user.",
                   {"assignee": text("Assignee name or email")}, ["assignee"]),
        definition("gmail_send_email", "Send an email immediately through the user's Gmail account.",
                   {"to": text("Recipient address"), "subject": text("Subject line"), "body": text("Body text")},
                   ["to", "subject"]),
        definition("gmail_search_messages", "Search the user's Gmail messages by query string.",
                   {"query": text("Gmail search query")}, ["query"]),
        definition("gcal_create_event", "Create a Google Calendar event with a start and end time.",
                   {"title": text("Event title"), "start": text("Start time ISO-8601"), "end": text("End time ISO-8601")},
                   ["title", "start", "end"]),
        definition("drive_search_files", "Search files in Google Drive by name keyword.",
                   {"query": text("File name keyword")}, ["query"]),
        definition("memory_store", "Store a short note in long-term memory under a key; does not send it anywhere.",
                   {"key": text("Memory key"), "value": text("Note value")}, ["key", "value"]),
    ]
    domains = [
        ("email", "email messages"), ("calendar", "calendar events"),
        ("contact", "address book contacts"), ("ticket", "customer support tickets"),
        ("invoice", "billing invoices"), ("order", "purchase orders"),
        ("shipment", "parcel shipments"), ("product", "shop products"),
        ("inventory", "warehouse inventory items"), ("meeting", "meeting reservations"),
        ("task", "team tasks"), ("document", "office documents"),
        ("wiki", "internal wiki pages"), ("repository", "Git repositories"),
        ("branch", "Git branches"), ("pull_request", "code pull requests"),
        ("container", "container instances"), ("deployment", "application deployments"),
        ("dashboard", "monitoring dashboards"), ("metric", "monitoring metric definitions"),
        ("alert_rule", "monitoring alert rules"), ("database", "database instances"),
        ("backup", "database backups"), ("image", "image library assets"),
        ("video", "video library assets"), ("playlist", "music playlists"),
        ("subscription", "service subscriptions"), ("permission", "access permission records"),
        ("project", "projects"), ("milestone", "project milestones"), ("sprint", "sprints"),
        ("epic", "epics"), ("bug", "bug reports"), ("release", "software releases"),
        ("incident", "incidents"), ("postmortem", "postmortems"), ("runbook", "runbooks"),
        ("dataset", "datasets"), ("model", "machine learning models"), ("experiment", "experiments"),
        ("notebook", "analysis notebooks"), ("pipeline", "data pipelines"), ("job", "batch jobs"),
        ("schedule", "scheduled jobs"), ("queue", "message queues"), ("webhook", "webhooks"),
        ("api_key", "API keys"), ("secret", "stored secrets"), ("certificate", "TLS certificates"),
        ("domain", "registered domains"), ("firewall_rule", "firewall rules"), ("vpn_peer", "VPN peers"),
        ("load_balancer", "load balancers"), ("cluster", "compute clusters"), ("node", "cluster nodes"),
        ("namespace", "namespaces"), ("volume", "storage volumes"), ("snapshot", "storage snapshots"),
        ("registry", "image registries"), ("build", "build records"), ("artifact", "build artifacts"),
        ("package", "software packages"), ("dependency", "dependency records"), ("license", "software licenses"),
        ("vendor", "vendors"), ("contract", "contracts"), ("quote", "sales quotes"),
        ("payment", "payments"), ("refund", "refunds"), ("payout", "payouts"),
        ("ledger", "ledger entries"), ("tax_rule", "tax rules"), ("campaign", "marketing campaigns"),
        ("segment", "audience segments"), ("audience", "audiences"), ("funnel", "conversion funnels"),
        ("coupon", "coupons"), ("review", "product reviews"), ("comment", "comments"),
        ("message", "messages"), ("thread", "conversation threads"), ("channel", "chat channels"),
        ("invitation", "invitations"), ("notification", "notification records"),
        ("template", "message templates"), ("glossary", "glossary terms"),
    ]
    generated = []
    for action in ("list", "get", "create"):
        for domain, label in domains:
            if action == "list":
                desc = f"List {label}, optionally matching a keyword; returns identifiers and names."
                props, required = {"keyword": text("Optional filtering keyword")}, []
            elif action == "get":
                desc = f"Get the detailed stored record of one item among {label} by its identifier."
                props, required = {"id": text(f"Identifier of the {domain}")}, ["id"]
            else:
                desc = f"Create a new record among {label} with a name and description."
                props, required = {"name": text("New record name"), "description": text("New record details")}, ["name", "description"]
            generated.append(definition(f"{action}_{domain}", desc, props, required))
    return nearby + popular + generated


def catalogs(real_tools, sizes):
    real_tools = sorted(real_tools, key=lambda t: t["name"])
    names = {t["name"] for t in real_tools}
    extra = [t for t in synthetic_tools() if t["name"] not in names]
    pool = real_tools + extra
    result = []
    for size in sorted(set([len(real_tools)] + list(sizes))):
        if size < len(real_tools) or size > len(pool):
            raise ValueError(f"目录规模 {size} 超出可用范围 {len(real_tools)}..{len(pool)}")
        result.append(pool[:size])
    return result


def cases():
    result = []

    def add(identifier, question, calls, split="test", kind="single"):
        result.append({"id": identifier, "question": question, "expected_calls": calls,
                       "split": split, "kind": kind})

    def call(name, **arguments):
        return {"name": name, "arguments": arguments}

    def weather(city, lat, lon):
        return call("get_weather", city=city, latitude=lat, longitude=lon)

    path = "/tmp/rag-eval/app.log"

    # ================================================== 单工具：真实插件（16）
    add("weather-1", f"查询北京现在的天气综合报告，包括温度和湿度。纬度39.9，经度116.4。",
        [weather("北京", "39.9", "116.4")], split="dev")
    add("weather-2", "帮我看看上海现在的天气情况，包括温度和湿度。坐标：纬度31.2、经度121.5。",
        [weather("上海", "31.2", "121.5")])
    add("weather-3", "查一下成都的温度、湿度等天气信息，经纬度是30.7（纬度）、104.1（经度）。",
        [weather("成都", "30.7", "104.1")])
    add("weather-4", "Please fetch a general weather report for 深圳, latitude 22.5, longitude 114.1.",
        [weather("深圳", "22.5", "114.1")])

    add("tail-1", f"显示文件{path}最后20行。", [call("tail_log", path=path, lines=20)], split="dev")
    add("tail-2", f"只看日志{path}末尾50行的原文，不做摘要。", [call("tail_log", path=path, lines=50)])
    add("tail-3", f"把{path}最近写入的10行取出来。", [call("tail_log", path=path, lines=10)])
    add("tail-4", f"Show the last 15 lines of {path}.", [call("tail_log", path=path, lines=15)])

    add("filter-1", f"在{path}最近200行中查找包含字面关键词timeout的日志。",
        [call("analyze_log", path=path, keyword="timeout", max_lines=200)], split="dev")
    add("filter-2", f"从{path}最近200行筛出包含connection的原始日志行。",
        [call("analyze_log", path=path, keyword="connection", max_lines=200)])
    add("filter-3", f"找出{path}最近200行中出现ERROR的行，保留原文。",
        [call("analyze_log", path=path, keyword="ERROR", max_lines=200)])
    add("filter-4", f"Find lines containing the literal keyword retry in the last 200 lines of {path}.",
        [call("analyze_log", path=path, keyword="retry", max_lines=200)])

    add("summary-1", f"汇总{path}最近500行中的WARN和ERROR，给出错误摘要。",
        [call("summarize_errors", path=path, max_lines=500)], split="dev")
    add("summary-2", f"给{path}最近500行的警告与错误做一份归纳。",
        [call("summarize_errors", path=path, max_lines=500)])
    add("summary-3", f"Summarize WARN and ERROR records in the last 500 lines of {path}.",
        [call("summarize_errors", path=path, max_lines=500)])
    add("summary-4", f"把{path}最近500行里的警告、错误整理成摘要。",
        [call("summarize_errors", path=path, max_lines=500)])

    # ================================================== 单工具：边界（2）
    add("boundary-1", "北京纬度39.900，经度116.400，请获取包含温度、湿度的最新天气综合报告。",
        [weather("北京", "39.9", "116.4")])
    add("boundary-2", f"概括{path}最近500行里的警告与错误，并归纳常见错误原因。",
        [call("summarize_errors", path=path, max_lines=500)])

    # ================================================== 单工具：近似能力（8）
    add("near-lines-1", f"统计{path}一共有多少行。", [call("count_log_lines", path=path)], split="dev")
    add("near-lines-2", f"读取{path}的全部内容，不要只读末尾几千行。", [call("read_entire_log", path=path)])
    add("near-lines-3", "把 /tmp/rag-eval/old.log 压缩成归档文件，不要分析里面的内容。",
        [call("archive_log", path="/tmp/rag-eval/old.log")])
    add("near-lines-4", f"用正则 ^2026- 搜索{path}里匹配的行。",
        [call("search_log_regex", path=path, pattern="^2026-")])
    add("near-lines-5", f"统计{path}里每种日志级别各有多少条。",
        [call("count_error_levels", path=path)])
    add("near-weather-1", "查询北京 2026-10-01 的历史天气观测数据。",
        [call("weather_history", city="北京", date="2026-10-01")])
    add("near-weather-2", "查一下北京现在的空气质量 PM2.5 和 PM10。", [call("air_quality", city="北京")])
    add("near-weather-3", "北京（纬度39.9，经度116.4）2026-10-27 的日出和日落时间分别是几点？",
        [call("sunrise_sunset", latitude="39.9", longitude="116.4", date="2026-10-27")])

    # ================================================== 单工具：跨应用（14）
    add("gh-search-1", "在仓库 acme/backend 里搜索包含 retryWithBackoff 的代码。",
        [call("github_search_code", query="retryWithBackoff", repo="acme/backend")])
    add("gh-issue-1", "给 acme/backend 仓库新建一个 issue，标题为“登录超时”。",
        [call("github_create_issue", repo="acme/backend", title="登录超时")], split="dev")
    add("slack-post-1", "在 Slack 的 #deploy 频道发一条消息：v1.4.2 已上线。",
        [call("slack_post_message", channel="#deploy", text="v1.4.2 已上线")])
    add("notion-search-1", "在 Notion 里搜索标题含“周会纪要”的页面。",
        [call("notion_search_pages", query="周会纪要")])
    add("fs-read-1", "读取本地文件 /data/config.json 的内容。",
        [call("fs_read_file", path="/data/config.json")], split="dev")
    add("fs-list-1", "列出 /tmp/rag-eval 目录下有哪些文件。",
        [call("fs_list_directory", path="/tmp/rag-eval")])
    add("pg-tables-1", "列出 Postgres 的 public schema 下有哪些表。",
        [call("postgres_list_tables", schema="public")])
    add("browser-nav-1", "用无头浏览器打开 https://example.com/pricing 并返回页面正文。",
        [call("browser_navigate", url="https://example.com/pricing")])
    add("web-search-1", "在网上搜索“MCP 工具检索 评测”相关的结果。",
        [call("web_search", query="MCP 工具检索 评测")])
    add("sentry-resolve-1", "把 Sentry 的 PROJ-1234 这个问题标记为已解决。",
        [call("sentry_resolve_issue", issue_id="PROJ-1234")])
    add("jira-create-1", "在 Jira 的 OPS 项目里建一个 Task，标题是“磁盘告警阈值调整”。",
        [call("jira_create_issue", project="OPS", summary="磁盘告警阈值调整", issue_type="Task")])
    add("gmail-send-1", "给 ops@example.com 发一封邮件，主题“周报”，正文写“见附件”。",
        [call("gmail_send_email", to="ops@example.com", subject="周报", body="见附件")])
    add("gcal-create-1", "在 Google 日历创建一个日程，标题“面试复盘”，2026-10-27T16:00:00 开始，2026-10-27T17:00:00 结束。",
        [call("gcal_create_event", title="面试复盘", start="2026-10-27T16:00:00", end="2026-10-27T17:00:00")])
    add("drive-search-1", "在 Google Drive 里搜文件名含“预算表”的文件。",
        [call("drive_search_files", query="预算表")])

    # ================================================== 多工具（24）
    add("multi-1", f"查询北京的天气（纬度39.9，经度116.4），并读取{path}最后20行。",
        [weather("北京", "39.9", "116.4"), call("tail_log", path=path, lines=20)],
        split="dev", kind="multi")
    add("multi-2", f"获取北京天气（纬度39.9，经度116.4），同时汇总{path}最近500行的WARN和ERROR。",
        [weather("北京", "39.9", "116.4"), call("summarize_errors", path=path, max_lines=500)], kind="multi")
    add("multi-3", f"看{path}最后50行，并统计这个文件一共有多少行。",
        [call("tail_log", path=path, lines=50), call("count_log_lines", path=path)], kind="multi")
    add("multi-4", f"在{path}最近200行里找出包含timeout的行，再汇总最近500行的警告与错误。",
        [call("analyze_log", path=path, keyword="timeout", max_lines=200),
         call("summarize_errors", path=path, max_lines=500)], kind="multi")
    add("multi-5", f"汇总{path}最近500行里的错误，再单独给出每种日志级别的数量。",
        [call("summarize_errors", path=path, max_lines=500), call("count_error_levels", path=path)], kind="multi")
    add("multi-6", f"显示{path}最后10行，然后再把这个文件的全文读一遍。",
        [call("tail_log", path=path, lines=10), call("read_entire_log", path=path)], kind="multi")
    add("multi-7", "打开 https://example.com/status 并截图保存到 /tmp/rag-eval/status.png。",
        [call("browser_navigate", url="https://example.com/status"),
         call("browser_screenshot", path="/tmp/rag-eval/status.png")], kind="multi")
    add("multi-8", "列出 /tmp/rag-eval 目录，并把文件清单追加到 Notion 页面 page-7f21 上。",
        [call("fs_list_directory", path="/tmp/rag-eval"),
         call("notion_append_blocks", page_id="page-7f21")], kind="multi")
    add("multi-9", "读取 /data/config.json，把内容追加到 Notion 页面 page-7f21 上。",
        [call("fs_read_file", path="/data/config.json"),
         call("notion_append_blocks", page_id="page-7f21")], kind="multi")
    add("multi-10", "给 acme/backend 建一个标题为“索引构建超时”的 issue，然后在 Slack 的 #infra 频道发一句“issue 已创建”。",
        [call("github_create_issue", repo="acme/backend", title="索引构建超时"),
         call("slack_post_message", channel="#infra", text="issue 已创建")],
        split="dev", kind="multi")
    add("multi-11", "看一下 Postgres 里 orders 表的结构，然后把结果发给 ops@example.com，主题写“orders 表结构”。",
        [call("postgres_describe_table", table="orders"),
         call("gmail_send_email", to="ops@example.com", subject="orders 表结构")],
        split="dev", kind="multi")
    add("multi-12", "在 Google Drive 搜“预算表”，然后把文件名发邮件给 finance@example.com，主题“预算表”。",
        [call("drive_search_files", query="预算表"),
         call("gmail_send_email", to="finance@example.com", subject="预算表")], kind="multi")
    add("multi-13", "列出 acme/backend 的 open PR，然后给这个仓库建一个标题为“评审待处理”的 issue。",
        [call("github_list_pull_requests", repo="acme/backend", state="open"),
         call("github_create_issue", repo="acme/backend", title="评审待处理")], kind="multi")
    add("multi-14", "列出 Sentry 项目 backend 未解决的 issue，再读一下 PROJ-1234 的堆栈详情。",
        [call("sentry_list_issues", project="backend"),
         call("sentry_get_issue", issue_id="PROJ-1234")],
        split="dev", kind="multi")
    add("multi-15", "在 acme/backend 里搜 retryWithBackoff 的代码，把结果整理成 Notion 页面“检索结果”，父页面是 page-7f21。",
        [call("github_search_code", query="retryWithBackoff", repo="acme/backend"),
         call("notion_create_page", parent_id="page-7f21", title="检索结果")],
        split="dev", kind="multi")
    add("multi-16", "搜一下“MCP 工具检索 评测”，再抓取 https://example.com/blog/rag 这个页面的正文。",
        [call("web_search", query="MCP 工具检索 评测"),
         call("web_fetch_page", url="https://example.com/blog/rag")], kind="multi")
    add("multi-17", "在 Jira 的 OPS 项目里建一个 Task“磁盘告警阈值调整”，然后在 Slack 的 #ops 频道说一句“已建单”。",
        [call("jira_create_issue", project="OPS", summary="磁盘告警阈值调整", issue_type="Task"),
         call("slack_post_message", channel="#ops", text="已建单")], kind="multi")
    add("multi-18", "在 Linear 的 Platform 团队建 issue“补充索引监控”，再建日程“索引评审”，2026-10-28T10:00:00 到 2026-10-28T11:00:00。",
        [call("linear_create_issue", team="Platform", title="补充索引监控"),
         call("gcal_create_event", title="索引评审", start="2026-10-28T10:00:00", end="2026-10-28T11:00:00")],
        kind="multi")
    add("multi-19", "把 Sentry 的 PROJ-1234 标记为已解决，然后在 Jira 的 OPS 项目建一个 Task“PROJ-1234 已修复”。",
        [call("sentry_resolve_issue", issue_id="PROJ-1234"),
         call("jira_create_issue", project="OPS", summary="PROJ-1234 已修复", issue_type="Task")], kind="multi")
    add("multi-20", "给 ops@example.com 发邮件主题“周报”正文“见附件”，再建日程“周会”，2026-10-29T14:00:00 到 2026-10-29T15:00:00。",
        [call("gmail_send_email", to="ops@example.com", subject="周报", body="见附件"),
         call("gcal_create_event", title="周会", start="2026-10-29T14:00:00", end="2026-10-29T15:00:00")],
        kind="multi")
    add("multi-21", "在 Notion 里搜标题含“周会纪要”的页面，再把结果发到 Slack 的 #team 频道，正文写“周会纪要已找到”。",
        [call("notion_search_pages", query="周会纪要"),
         call("slack_post_message", channel="#team", text="周会纪要已找到")], kind="multi")
    add("multi-22", "看 acme/backend 里提交 9f3c1a2 改了什么，再发到 Slack 的 #review 频道，正文写“提交已看完”。",
        [call("github_get_commit", repo="acme/backend", sha="9f3c1a2"),
         call("slack_post_message", channel="#review", text="提交已看完")], kind="multi")
    add("multi-23", "列出 /tmp/rag-eval 目录，并统计 /tmp/rag-eval/app.log 一共有多少行。",
        [call("fs_list_directory", path="/tmp/rag-eval"),
         call("count_log_lines", path=path)], kind="multi")
    add("multi-24", "查北京现在的天气（纬度39.9，经度116.4），再看一下北京现在的空气质量 PM2.5。",
        [weather("北京", "39.9", "116.4"), call("air_quality", city="北京")], kind="multi")

    # ================================================== 无工具（16）
    add("no-tool-1", "你好，你能帮我做什么？", [], split="dev", kind="no_tool")
    add("no-tool-2", "解释一下 JSON-RPC 里的 id 字段是做什么用的。", [], kind="no_tool")
    add("no-tool-3", "把 hello world 翻译成中文。", [], kind="no_tool")
    add("no-tool-4", "1 加到 100 等于多少？", [], kind="no_tool")
    add("no-tool-5", "讲一个和技术无关的简短笑话。", [], split="dev", kind="no_tool")
    add("no-tool-6", "什么是 MCP 协议？用两句话说明，不需要查资料。", [], kind="no_tool")
    add("no-tool-7", "帮我写一句 Python 的 Hello World。", [], split="dev", kind="no_tool")
    add("no-tool-8", "解释一下倒排索引是什么，口述即可。", [], kind="no_tool")
    add("no-tool-9", "把“今天的实验跑完了”翻译成英文。", [], kind="no_tool")
    add("no-tool-10", "2 的 10 次方是多少？", [], kind="no_tool")
    add("no-tool-11", "简单说说 RAG 是什么，不用查资料。", [], kind="no_tool")
    add("no-tool-12", "给“向量检索”这个主题起三个标题。", [], kind="no_tool")
    add("no-tool-13", "什么是 TCP 三次握手？直接讲原理。", [], kind="no_tool")
    add("no-tool-14", "把我们刚才聊的内容用一句话总结一下。", [], kind="no_tool")
    add("no-tool-15", "你是什么模型？简单介绍一下自己。", [], kind="no_tool")
    add("no-tool-16", "用一句话解释 LRU 缓存。", [], kind="no_tool")
    return result


CITY_ALIASES = {"北京": ["Beijing"], "上海": ["Shanghai"], "广州": ["Guangzhou"],
                "深圳": ["Shenzhen"], "成都": ["Chengdu"], "杭州": ["Hangzhou"]}
