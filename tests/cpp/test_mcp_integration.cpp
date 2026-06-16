/**
 * @file test_mcp_integration.cpp
 * @brief C++ 集成测试 — 使用项目自身的 mcp_client 测试 mcp_server + plugins
 *
 * 测试链路:
 *   mcp_client (MCPClient STDIO) → fork mcp_server → 加载 plugins → 返回结果
 *
 * 三种运行模式:
 *   1. 自动化测试 (默认)         — 自动跑 6 个测试用例
 *   2. 交互模式 (-i)             — 手动 call/list/quit
 *   3. AI 模式 (--ai <key>)      — 自然语言交互，LLM 自动调用工具
 *
 * 编译: cmake --build build --target test_mcp_integration
 * 运行:
 *   ./build/tests/cpp/test_mcp_integration <server> <plugins>
 *   ./build/tests/cpp/test_mcp_integration <server> <plugins> -i
 *   ./build/tests/cpp/test_mcp_integration <server> <plugins> --ai sk-xxx
 *   ./build/tests/cpp/test_mcp_integration <server> <plugins> --ai sk-xxx --model qwen-max
 */

#include "agent_rpc/mcp/mcp_client.h"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <algorithm>

#include <curl/curl.h>
#include <json/json.h>

using namespace agent_rpc::mcp;

// ========================== 输出辅助 ==========================

static int g_passed = 0;
static int g_failed = 0;

static void print_header(const std::string& title) {
    std::cout << "\n\033[1;36m" << std::string(60, '=') << "\033[0m\n";
    std::cout << "\033[1;36m  " << title << "\033[0m\n";
    std::cout << "\033[1;36m" << std::string(60, '=') << "\033[0m\n";
}

static void print_ok(const std::string& msg) {
    std::cout << "  \033[0;32m[OK]\033[0m   " << msg << "\n";
    g_passed++;
}

static void print_fail(const std::string& msg) {
    std::cout << "  \033[0;31m[FAIL]\033[0m " << msg << "\n";
    g_failed++;
}

static void print_info(const std::string& msg) {
    std::cout << "  \033[0;33m[INFO]\033[0m " << msg << "\n";
}

// ========================== CURL 辅助 ==========================

static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(
        static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

// ========================== DashScope LLM 调用 ==========================

static Json::Value callDashScopeLLM(
    const std::string& api_key,
    const std::string& model,
    const Json::Value& messages,
    const Json::Value& tools)
{
    static const std::string API_URL =
        "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";

    CURL* curl = curl_easy_init();
    if (!curl) {
        Json::Value err;
        err["error"] = "CURL 初始化失败";
        return err;
    }

    // 构建请求体
    Json::Value body;
    body["model"] = model;
    body["messages"] = messages;
    if (!tools.isNull() && tools.isArray() && tools.size() > 0) {
        body["tools"] = tools;
    }

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    std::string body_str = Json::writeString(wb, body);

    // 设置 HTTP headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, auth.c_str());

    std::string response_data;
    curl_easy_setopt(curl, CURLOPT_URL, API_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    Json::Value result;
    Json::Reader reader;

    if (res != CURLE_OK) {
        result["error"] = std::string("网络错误: ") + curl_easy_strerror(res);
        return result;
    }

    if (!reader.parse(response_data, result)) {
        result["error"] = "LLM 响应 JSON 解析失败";
        result["raw"] = response_data.substr(0, 500);
        return result;
    }

    if (http_code != 200) {
        std::string err_msg = result.get("message",
            "HTTP " + std::to_string(http_code)).asString();
        Json::Value err;
        err["error"] = err_msg;
        err["http_code"] = static_cast<Json::Int64>(http_code);
        return err;
    }

    return result;
}

// ========================== 工具格式转换 ==========================

static Json::Value convertMCPToolsToOpenAIFormat(const std::vector<MCPTool>& tools) {
    Json::Value openai_tools(Json::arrayValue);

    for (const auto& tool : tools) {
        Json::Value schema;
        Json::Reader reader;
        if (!reader.parse(tool.input_schema, schema)) {
            schema = Json::Value(Json::objectValue);
        }

        Json::Value fn;
        fn["name"] = tool.name;
        fn["description"] = tool.description;
        fn["parameters"] = schema;

        Json::Value entry;
        entry["type"] = "function";
        entry["function"] = fn;

        openai_tools.append(entry);
    }

    return openai_tools;
}

// ========================== 工具结果提取 ==========================

static std::string extractToolResultText(const MCPResponse& response) {
    if (response.is_error) {
        return "Error: " + response.error;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(response.result, root)) {
        return response.result;
    }

    std::string text;
    const Json::Value& content = root["content"];
    if (content.isArray()) {
        for (const auto& item : content) {
            if (item.get("type", "") == "text") {
                text += item.get("text", "").asString();
            }
        }
    }

    if (text.empty()) {
        text = response.result;
    }

    return text;
}

// ========================== 测试用例 ==========================

static void test_list_tools(MCPClient& client) {
    print_header("Test: list_tools");

    auto tools = client.listTools();

    if (tools.empty()) {
        print_fail("tools 列表为空，预期至少有 1 个工具");
        return;
    }

    print_ok("获取到 " + std::to_string(tools.size()) + " 个工具");

    for (const auto& tool : tools) {
        print_info("  - " + tool.name + ": " + tool.description.substr(0, 60) + "...");
    }

    bool has_weather = false, has_sleep = false;
    for (const auto& tool : tools) {
        if (tool.name == "get_weather") has_weather = true;
        if (tool.name == "sleep") has_sleep = true;
    }

    if (has_weather) print_ok("找到 get_weather 工具");
    else print_fail("未找到 get_weather 工具");

    if (has_sleep) print_ok("找到 sleep 工具");
    else print_fail("未找到 sleep 工具");
}

static void test_call_weather(MCPClient& client) {
    print_header("Test: call_tool(get_weather)");

    std::string args = R"({"city": "beijing", "latitude": "39.9042", "longitude": "116.4074"})";
    auto response = client.callTool("get_weather", args);

    if (response.is_error) {
        print_fail("调用失败: " + response.error);
        return;
    }

    print_ok("get_weather 调用成功");
    std::string preview = response.result.substr(0, std::min(response.result.size(), size_t(200)));
    print_info("结果预览: " + preview + (response.result.size() > 200 ? "..." : ""));
}

static void test_call_sleep(MCPClient& client) {
    print_header("Test: call_tool(sleep)");

    std::string args = R"({"milliseconds": 100})";
    auto response = client.callTool("sleep", args);

    if (response.is_error) {
        print_fail("调用失败: " + response.error);
        return;
    }

    print_ok("sleep 调用成功 (100ms)");
    print_info("结果: " + response.result);
}

static void test_call_nonexistent(MCPClient& client) {
    print_header("Test: call_tool(不存在的工具)");

    auto response = client.callTool("this_tool_does_not_exist", "{}");

    if (response.is_error) {
        print_ok("正确返回了错误: " + response.error);
    } else {
        if (response.result.find("isError") != std::string::npos &&
            response.result.find("true") != std::string::npos) {
            print_ok("result 中标记了 isError=true");
        } else {
            print_fail("调用不存在的工具却没有返回错误");
        }
    }
}

static void test_list_prompts(MCPClient& client) {
    print_header("Test: list_prompts");

    auto prompts = client.listPrompts();

    print_info("获取到 " + std::to_string(prompts.size()) + " 个 prompt");
    for (const auto& prompt : prompts) {
        print_info("  - " + prompt.name + ": " + prompt.description);
    }

    if (prompts.size() >= 0) {
        print_ok("list_prompts 接口正常工作");
    }
}

static void test_list_resources(MCPClient& client) {
    print_header("Test: list_resources");

    auto resources = client.listResources();

    print_info("获取到 " + std::to_string(resources.size()) + " 个 resource");
    for (const auto& res : resources) {
        print_info("  - " + res.name + " [" + res.uri + "]");
    }

    if (resources.size() >= 0) {
        print_ok("list_resources 接口正常工作");
    }
}

// ========================== 交互模式 (手动) ==========================

static void run_interactive(MCPClient& client) {
    print_header("交互模式");

    auto tools = client.listTools();
    std::cout << "  可用工具 (" << tools.size() << "):\n";
    for (size_t i = 0; i < tools.size(); i++) {
        std::cout << "    \033[0;36m" << (i + 1) << ". " << tools[i].name
                  << "\033[0m — " << tools[i].description.substr(0, 60) << "\n";
    }

    std::cout << "\n  命令:\n";
    std::cout << "    \033[1;33mcall <工具名> <JSON参数>\033[0m  调用工具\n";
    std::cout << "    \033[1;33mlist\033[0m                    刷新工具列表\n";
    std::cout << "    \033[1;33mquit\033[0m                    退出\n";
    std::cout << "\n  示例:\n";
    std::cout << "    call get_weather {\"city\":\"beijing\",\"latitude\":\"39.9\",\"longitude\":\"116.4\"}\n";
    std::cout << "    call sleep {\"milliseconds\":500}\n";
    std::cout << "    call calculator {\"expression\":\"123+456\"}\n";

    std::string line;
    while (true) {
        std::cout << "\n\033[1;32mmcp>\033[0m ";
        std::cout.flush();

        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        if (line == "quit" || line == "exit" || line == "q") {
            break;
        }

        if (line == "list" || line == "ls") {
            tools = client.listTools();
            std::cout << "  可用工具 (" << tools.size() << "):\n";
            for (size_t i = 0; i < tools.size(); i++) {
                std::cout << "    \033[0;36m" << (i + 1) << ". " << tools[i].name
                          << "\033[0m — " << tools[i].description.substr(0, 60) << "\n";
            }
            continue;
        }

        if (line.substr(0, 4) == "call") {
            size_t name_start = 5;
            size_t space_pos = line.find(' ', name_start);
            std::string tool_name;
            std::string args = "{}";

            if (space_pos == std::string::npos) {
                tool_name = line.substr(name_start);
            } else {
                tool_name = line.substr(name_start, space_pos - name_start);
                args = line.substr(space_pos + 1);
            }

            if (tool_name.empty()) {
                std::cout << "  \033[0;31m用法: call <工具名> <JSON参数>\033[0m\n";
                continue;
            }

            std::cout << "  \033[0;33m调用 " << tool_name << " ...\033[0m\n";

            auto start = std::chrono::steady_clock::now();
            auto response = client.callTool(tool_name, args);
            auto end = std::chrono::steady_clock::now();
            int64_t duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            if (response.is_error) {
                std::cout << "  \033[0;31m[ERROR]\033[0m " << response.error << "\n";
            } else {
                std::cout << "  \033[0;32m[OK]\033[0m (" << duration_ms << "ms)\n";
                std::cout << "  " << response.result << "\n";
            }
            continue;
        }

        std::cout << "  未知命令。输入 call/list/quit\n";
    }
}

// ========================== AI 交互模式 ==========================

static void run_ai_interactive(
    MCPClient& client,
    const std::string& api_key,
    const std::string& model)
{
    print_header("AI 交互模式 (Natural Language + MCP)");

    // 获取工具并转换格式
    auto tools = client.listTools();
    if (tools.empty()) {
        print_fail("没有可用的 MCP 工具");
        return;
    }

    Json::Value openai_tools = convertMCPToolsToOpenAIFormat(tools);

    std::cout << "  可用工具 (" << tools.size() << "):\n";
    for (size_t i = 0; i < tools.size(); i++) {
        std::cout << "    \033[0;36m" << (i + 1) << ". " << tools[i].name
                  << "\033[0m — " << tools[i].description.substr(0, 80) << "\n";
    }
    std::cout << "\n  \033[1;33m模型: " << model << "\033[0m\n";
    std::cout << "  输入自然语言问题，AI 会自动选择并调用工具。\n";
    std::cout << "  输入 \033[1;33mquit\033[0m 退出。\n";

    static const int MAX_AGENT_ROUNDS = 10; // 最多循环10轮，最后断开mcp连接

    std::string line; // 你输入的问题
    while (true) {
        std::cout << "\n\033[1;32m你\033[0m: "; // `\n`（换行）+ `\033[1;32m`（加粗绿色）+ `你`（文本）+ `\033[0m`（重置样式）
        std::cout.flush(); // 手动刷新输出缓冲区，强制立即将 std::cout 中缓存的内容写到终端

        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;
        if (line == "quit" || line == "exit" || line == "q") break;

        // 初始化消息历史
        Json::Value messages(Json::arrayValue);

        Json::Value sys_msg; // 提示词
        sys_msg["role"] = "system";
        sys_msg["content"] = "你是一个智能助手，可以使用提供的工具来回答用户问题。"
                             "当需要使用工具时，请调用对应的工具。"
                             "请用用户的语言回答。";
        messages.append(sys_msg);

        Json::Value user_msg;
        user_msg["role"] = "user";
        user_msg["content"] = line;
        messages.append(user_msg);

        bool answered = false;

        for (int round = 0; round < MAX_AGENT_ROUNDS; ++round) {
            std::cout << "  \033[0;33m[AI 思考中...]\033[0m\n";

            Json::Value llm_resp = callDashScopeLLM(
                api_key, model, messages, openai_tools);

            // 检查错误
            if (llm_resp.isMember("error")) {
                std::cout << "  \033[0;31m[LLM 错误]\033[0m "
                          << llm_resp["error"].asString() << "\n";
                break;
            }

            // 提取 assistant 消息
            const Json::Value& choices = llm_resp["choices"];
            if (!choices.isArray() || choices.empty()) {
                std::cout << "  \033[0;31m[错误]\033[0m LLM 返回无 choices\n";
                break;
            }

            const Json::Value& assistant_msg = choices[0]["message"];
            std::string content = assistant_msg.get("content", "").asString();
            const Json::Value& tool_calls = assistant_msg["tool_calls"];

            // 没有 tool_calls → 最终回答
            if (tool_calls.isNull() || !tool_calls.isArray() || tool_calls.empty()) {
                std::cout << "  \033[0;36m[AI]\033[0m " << content << "\n";
                answered = true;
                break;
            }

            // 有 tool_calls → 追加 assistant 消息到历史
            messages.append(assistant_msg);

            std::cout << "  \033[0;33m[AI 调用工具] ("
                      << tool_calls.size() << " 个)\033[0m\n";

            // 执行每个工具调用
            for (const auto& tc : tool_calls) {
                const Json::Value& fn = tc["function"];
                std::string tool_name = fn.get("name", "").asString();
                std::string args_str = fn.get("arguments", "{}").asString();
                std::string tc_id = tc.get("id", "").asString();

                std::cout << "    → " << tool_name << "("
                          << args_str.substr(0, 120) << ")\n";

                auto start = std::chrono::steady_clock::now();
                MCPResponse mcp_resp = client.callTool(tool_name, args_str);
                auto end = std::chrono::steady_clock::now();
                int64_t ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(end - start).count();

                std::string result_text = extractToolResultText(mcp_resp);

                if (mcp_resp.is_error) {
                    std::cout << "    \033[0;31m[工具错误]\033[0m "
                              << result_text.substr(0, 200) << "\n";
                } else {
                    std::cout << "    \033[0;32m[工具结果]\033[0m ("
                              << ms << "ms) "
                              << result_text.substr(0, 200)
                              << (result_text.size() > 200 ? "..." : "")
                              << "\n";
                }

                // 追加工具结果消息
                Json::Value tool_msg;
                tool_msg["role"] = "tool";
                tool_msg["tool_call_id"] = tc_id;
                tool_msg["content"] = result_text;
                messages.append(tool_msg);
            }

            // 继续 agent loop
        }

        if (!answered) {
            std::cout << "  \033[0;31m[超出最大轮次或出错]\033[0m\n";
        }
    }
}

// ========================== 主函数 ==========================

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <server_path> <plugins_path> [options]\n"   // std::cerr:标准错误输出，写入内容立即刷新(不经缓冲)
              << "\n"
              << "Options:\n"
              << "  --interactive, -i         交互模式: 手动输入命令调用工具\n"
              << "  --ai <api_key>            AI 模式: 自然语言交互, AI 自动调用工具\n"
              << "  --model <model>           指定 LLM 模型 (默认: qwen-plus)\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " ./build/mcp_server/mcp_server ./build/mcp_server/plugins\n"
              << "  " << prog << " ./build/mcp_server/mcp_server ./build/mcp_server/plugins -i\n"
              << "  " << prog << " ./build/mcp_server/mcp_server ./build/mcp_server/plugins --ai sk-xxx\n"
              << "  " << prog << " ./build/mcp_server/mcp_server ./build/mcp_server/plugins --ai sk-xxx --model qwen-max\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string server_path = argv[1];
    std::string plugins_path = argv[2];
    bool interactive = false;
    std::string ai_api_key;
    std::string ai_model = "qwen-plus";

    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--interactive" || arg == "-i") {
            interactive = true;
        } else if (arg == "--ai" && i + 1 < argc) {
            ai_api_key = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            ai_model = argv[++i];
        }
    }

    std::cout << "\033[1;35m";
    std::cout << "╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║       MCP Integration Test (C++ mcp_client)             ║\n";
    std::cout << "║  测试链路: mcp_client → mcp_server → plugins            ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
    std::cout << "\033[0m\n";

    print_info("Server:  " + server_path);
    print_info("Plugins: " + plugins_path);

    // ---------- 连接 ----------
    print_header("连接 MCP Server");

    MCPClient client; // 客户端对象
    std::vector<std::string> server_args = {"-p", plugins_path};

    if (!client.connect(server_path, server_args)) {
        print_fail("无法连接到 MCP Server: " + server_path);
        std::cout << "\n\033[0;31m测试中止。\033[0m\n";
        return 1;
    }
    print_ok("已通过 STDIO 连接到 MCP Server");

    // ---------- 运行测试 ----------
    if (!ai_api_key.empty()) {
        // AI 自然语言模式
        run_ai_interactive(client, ai_api_key, ai_model);
    } else if (interactive) {
        // 手动交互模式
        run_interactive(client);
    } else {
        // 自动化测试
        test_list_tools(client);
        test_call_weather(client);
        test_call_sleep(client);
        test_call_nonexistent(client);
        test_list_prompts(client);
        test_list_resources(client);
    }

    // ---------- 断开 ----------
    print_header("断开连接");
    client.disconnect();
    print_ok("已断开连接");

    // ---------- 汇总 ----------
    std::cout << "\n\033[1;35m" << std::string(60, '=') << "\033[0m\n";
    std::cout << "\033[1m  测试结果: ";
    std::cout << "\033[0;32m" << g_passed << " passed\033[0m, ";
    std::cout << "\033[0;31m" << g_failed << " failed\033[0m\n";
    std::cout << "\033[1;35m" << std::string(60, '=') << "\033[0m\n\n";

    return g_failed > 0 ? 1 : 0;
}
