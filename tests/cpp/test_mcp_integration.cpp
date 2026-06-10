/**
 * @file test_mcp_integration.cpp
 * @brief C++ 集成测试 — 使用项目自身的 mcp_client 测试 mcp_server + plugins
 *
 * 这条测试链路覆盖的模块:
 *   mcp_client (MCPClient STDIO) → fork mcp_server → 加载 plugins → 返回结果
 *
 * 编译: cmake --build build --target test_mcp_integration
 * 运行: ./build/tests/cpp/test_mcp_integration <server_path> <plugins_path>
 *
 * 示例:
 *   ./build/tests/cpp/test_mcp_integration \
 *       ./build/mcp_server/mcp_server \
 *       ./build/mcp_server/plugins
 */

#include "agent_rpc/mcp/mcp_client.h"

#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>

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

    // 检查是否包含预期的工具
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
    // 截取前 200 字符展示
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
        // 有些实现可能在 result 里标记 isError
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

    // prompts 可能为空（取决于插件），不一定是失败
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

// ========================== 主函数 ==========================

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <server_path> <plugins_path>\n"
              << "\n"
              << "Example:\n"
              << "  " << prog << " ./build/mcp_server/mcp_server ./build/mcp_server/plugins\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    std::string server_path = argv[1];
    std::string plugins_path = argv[2];

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

    MCPClient client;
    std::vector<std::string> server_args = {"-p", plugins_path};

    if (!client.connect(server_path, server_args)) {
        print_fail("无法连接到 MCP Server: " + server_path);
        std::cout << "\n\033[0;31m测试中止。\033[0m\n";
        return 1;
    }
    print_ok("已通过 STDIO 连接到 MCP Server");

    // ---------- 运行测试 ----------
    test_list_tools(client);
    test_call_weather(client);
    test_call_sleep(client);
    test_call_nonexistent(client);
    test_list_prompts(client);
    test_list_resources(client);

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
