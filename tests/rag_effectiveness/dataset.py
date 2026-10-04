"""Reviewed labels + synthetic tool metadata. No external data or business API calls."""


def definition(name, description, properties, required):
    return {"name": name, "description": description, "parameters": {
        "type": "object", "properties": properties, "required": required,
        "additionalProperties": False}}


def text(description):
    return {"type": "string", "description": description}


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
    return nearby + generated


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

    cities = [
        ("北京", "39.9", "116.4"), ("上海", "31.2", "121.5"),
        ("广州", "23.1", "113.3"), ("深圳", "22.5", "114.1"),
        ("成都", "30.7", "104.1"), ("杭州", "30.3", "120.2"),
    ]
    weather_templates = [
        "查询{city}的天气综合报告。纬度{lat}，经度{lon}。",
        "帮我看看{city}现在的天气情况，包括温度和湿度。坐标：纬度{lat}、经度{lon}。",
        "我要去{city}，请获取当地天气报告。纬度为{lat}，经度为{lon}。",
        "查一下{city}的温度、湿度等天气信息，经纬度是{lat}（纬度）、{lon}（经度）。",
        "Please fetch a general weather report for {city}, latitude {lat}, longitude {lon}.",
        "给我一份{city}最新的天气情况，纬度{lat}，经度{lon}。",
    ]
    for i, ((city, lat, lon), template) in enumerate(zip(cities, weather_templates)):
        add(f"weather-{i+1}", template.format(city=city, lat=lat, lon=lon),
            [call("get_weather", city=city, latitude=lat, longitude=lon)], "dev" if i == 0 else "test")

    path = "/tmp/rag-eval/app.log"
    tail_templates = [
        "显示文件{path}最后{n}行。", "只看日志{path}末尾{n}行的原文。",
        "把{path}最近写入的{n}行取出来。", "输出{path}末尾的{n}行日志，不做摘要。",
        "Show the last {n} lines of {path}.", "我想检查{path}的日志尾部，给我最后{n}行。",
    ]
    for i, template in enumerate(tail_templates):
        n = [20, 50, 10, 30, 15, 40][i]
        add(f"tail-{i+1}", template.format(path=path, n=n), [call("tail_log", path=path, lines=n)],
            "dev" if i == 0 else "test")

    keywords = ["timeout", "connection", "retry", "ERROR", "refused", "failed"]
    filter_templates = [
        "在{path}最近200行中查找包含字面关键词{word}的日志。",
        "从{path}最近200行筛出包含{word}的原始日志行。",
        "帮我过滤{path}最近200行中带{word}这个词的记录。",
        "找出{path}最近200行中出现{word}的行，保留原文。",
        "Find lines containing the literal keyword {word} in the last 200 lines of {path}.",
        "看看{path}最近200行有哪些日志包含{word}。",
    ]
    for i, (word, template) in enumerate(zip(keywords, filter_templates)):
        add(f"filter-{i+1}", template.format(path=path, word=word),
            [call("analyze_log", path=path, keyword=word, max_lines=200)], "dev" if i == 0 else "test")

    summary_templates = [
        "汇总{path}最近500行中的WARN和ERROR，给出错误摘要。",
        "给{path}最近500行的警告与错误做一份归纳。",
        "分析{path}最近500行的异常，汇总WARN/ERROR内容和常见错误。",
        "请概括{path}最近500行有哪些警告和错误。",
        "Summarize WARN and ERROR records in the last 500 lines of {path}.",
        "把{path}最近500行里的警告、错误整理成摘要。",
    ]
    for i, template in enumerate(summary_templates):
        add(f"summary-{i+1}", template.format(path=path),
            [call("summarize_errors", path=path, max_lines=500)], "dev" if i == 0 else "test")

    weather = call("get_weather", city="北京", latitude="39.9", longitude="116.4")
    add("multi-1", f"查询北京的天气报告（纬度39.9，经度116.4），并读取{path}最后20行。",
        [weather, call("tail_log", path=path, lines=20)], split="dev", kind="multi")
    add("multi-2", f"获取北京天气（纬度39.9，经度116.4），同时汇总{path}最近500行的WARN和ERROR。",
        [weather, call("summarize_errors", path=path, max_lines=500)], kind="multi")
    add("no-tool-1", "你好，请用一句话介绍你能如何帮助我。", [], split="dev", kind="no_tool")
    add("no-tool-2", "请简单解释什么是JSON-RPC，不需要查阅实时资料。", [], kind="no_tool")
    add("boundary-1", "北京纬度39.900，经度116.400，请获取包含温度、湿度的最新天气综合报告。", [weather])
    add("boundary-2", f"概括{path}最近500行里的警告与错误，并归纳常见错误原因。",
        [call("summarize_errors", path=path, max_lines=500)])
    return result


CITY_ALIASES = {"北京": ["Beijing"], "上海": ["Shanghai"], "广州": ["Guangzhou"],
                "深圳": ["Shenzhen"], "成都": ["Chengdu"], "杭州": ["Hangzhou"]}
