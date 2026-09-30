// Live stdio + Tool chain: C++ integration/RAG -> model -> integration -> plugin -> model.
// Requires DASHSCOPE_API_KEY. External embedding and chat services are test dependencies.
#include "agent_rpc/mcp/mcp_agent_integration.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using agent_rpc::mcp::MCPAgentConfig;
using agent_rpc::mcp::MCPAgentIntegration;
using agent_rpc::mcp::ToolInfo;
using json = nlohmann::json;

namespace {
constexpr int kTopK = 3;
constexpr const char* kQuestion =
    "Using a math tool, calculate 123 times 456. Base your answer on the tool result "
    "and reply with the numeric answer.";
constexpr const char* kChatUrl =
    "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

size_t writeResponse(char* data, size_t size, size_t count, void* target) {
    const size_t bytes = size * count;
    static_cast<std::string*>(target)->append(data, bytes);
    return bytes;
}

json askModel(const std::string& key, const std::string& model,
              const json& messages, const json& tools) {
    CURL* curl = curl_easy_init();
    require(curl != nullptr, "cannot initialize libcurl");
    json request = {{"model", model}, {"messages", messages}};
    if (!tools.empty()) {
        request["tools"] = tools;
        request["tool_choice"] = "auto";
    }
    const std::string body = request.dump();
    const std::string authorization = "Authorization: Bearer " + key;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, authorization.c_str());
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, kChatUrl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    const CURLcode status = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    require(status == CURLE_OK, std::string("chat request failed: ") + curl_easy_strerror(status));
    // Do not print the response body: a provider error can contain private request data.
    require(http_status == 200, "chat service returned HTTP " + std::to_string(http_status));
    return json::parse(response);
}

json firstMessage(const json& response) {
    require(response.contains("choices") && response["choices"].is_array() &&
                !response["choices"].empty() &&
                response["choices"][0].contains("message"),
            "chat response has no assistant message");
    return response["choices"][0]["message"];
}

std::string pluginText(const std::string& raw) {
    const json result = json::parse(raw);
    require(result.is_object() && result.value("isError", false) == false,
            "plugin reported an error");
    require(result.contains("content") && result["content"].is_array(),
            "plugin result has no content array");
    std::string text;
    for (const auto& part : result["content"]) {
        if (part.is_object() && part.value("type", "") == "text") {
            text += part.value("text", "");
        }
    }
    require(!text.empty(), "plugin returned no text");
    return text;
}

void run(const std::string& server, const std::string& plugins,
         const std::string& key, const std::string& model) {
    MCPAgentConfig config;
    config.enable_mcp = true;
    config.mcp_server_path = server;
    config.mcp_args = {"-p", plugins, "-l", std::filesystem::temp_directory_path().string()};
    config.max_retry_count = 0;
    config.rag_config.enabled = true;
    config.rag_config.top_k = kTopK;
    config.rag_config.similarity_threshold = 0.0f;
    // EmbeddingService reads DASHSCOPE_API_KEY itself; the key is never logged.

    MCPAgentIntegration integration;
    require(integration.initialize(config) && integration.isAvailable(),
            "stdio MCP connection failed");
    const auto all = integration.getAvailableTools();
    require(all.size() > kTopK, "need more discovered tools than RAG top_k");
    require(integration.hasToolAvailable("calculator") &&
                integration.hasToolAvailable("multiply"),
            "calculator plugin was not discovered");
    require(integration.isRAGEnabled(), "RAG failed to initialize or index tools");
    std::cout << "[PASS] stdio connected; " << all.size() << " tools discovered; RAG indexed\n";

    const auto selected = integration.getRelevantTools(kQuestion);
    require(!selected.empty() && selected.size() <= kTopK && selected.size() < all.size(),
            "RAG did not return a nonempty filtered subset");
    std::set<std::string> selected_names;
    for (const ToolInfo& tool : selected) {
        require(integration.hasToolAvailable(tool.name), "RAG returned an unknown tool");
        selected_names.insert(tool.name);
    }
    require(selected_names.count("calculator") || selected_names.count("multiply"),
            "RAG missed both calculator and multiply");
    std::cout << "[PASS] C++ RAG selected " << selected.size() << " of " << all.size() << " tools:";
    for (const auto& name : selected_names) std::cout << " " << name;
    std::cout << "\n";

    // The integration layer owns discovery, retrieval and calls. This test only adapts
    // its function format to the provider's Chat Completions envelope.
    const json functions = json::parse(MCPAgentIntegration::toFunctionCallingFormat(selected));
    json model_tools = json::array();
    for (const auto& function : functions) {
        model_tools.push_back({{"type", "function"}, {"function", function}});
    }
    json messages = json::array({
        {{"role", "system"}, {"content", "Use an offered math tool before answering. "
                                           "Do not calculate from memory. Answer with the tool's result."}},
        {{"role", "user"}, {"content", kQuestion}}
    });
    const json assistant = firstMessage(askModel(key, model, messages, model_tools));
    require(assistant.contains("tool_calls") && assistant["tool_calls"].is_array() &&
                !assistant["tool_calls"].empty(),
            "model did not request a tool");
    std::cout << "[PASS] external model requested " << assistant["tool_calls"].size()
              << " selected tool(s)\n";

    messages.push_back(assistant);
    bool verified_result = false;
    for (const auto& call : assistant["tool_calls"]) {
        require(call.contains("id") && call["id"].is_string() &&
                    call.contains("function") && call["function"].is_object(),
                "malformed model tool call");
        const auto& function = call["function"];
        const std::string name = function.value("name", "");
        require(selected_names.count(name) != 0, "model requested a tool outside RAG selection");
        require(function.contains("arguments") && function["arguments"].is_string(),
                "model tool arguments are missing");
        const json arguments = json::parse(function["arguments"].get<std::string>());
        require(arguments.is_object(), "model tool arguments are not an object");
        const auto tool_result = integration.callTool(name, arguments.dump());
        require(tool_result.success, "MCPAgentIntegration::callTool failed: " + tool_result.error);
        const std::string text = pluginText(tool_result.result);
        if (text.find("56088") != std::string::npos) verified_result = true;
        messages.push_back({{"role", "tool"}, {"tool_call_id", call["id"]},
                            {"content", text}});
        std::cout << "[PASS] integration -> manager -> client -> server -> plugin: "
                  << name << " returned " << text << "\n";
    }
    require(verified_result, "plugin result did not contain the expected value 56088");

    const json final_message = firstMessage(askModel(key, model, messages, json::array()));
    require(!final_message.contains("tool_calls") || final_message["tool_calls"].is_null() ||
                final_message["tool_calls"].empty(),
            "model requested another tool instead of a final answer");
    require(final_message.contains("content") && final_message["content"].is_string(),
            "model gave no final text");
    const std::string answer = final_message["content"].get<std::string>();
    require(answer.find("56088") != std::string::npos,
            "model final answer did not include the verified result");
    std::cout << "[PASS] external model final answer: " << answer << "\n";
    integration.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <mcp_server> <calculator_plugins_dir> [chat_model]\n"
                  << "Requires DASHSCOPE_API_KEY in the environment.\n";
        return 2;
    }
    const char* key = std::getenv("DASHSCOPE_API_KEY");
    if (!key || !*key) {
        std::cerr << "[SKIP] DASHSCOPE_API_KEY is absent; live embedding/chat chain not run\n";
        return 77;
    }
    try {
        run(argv[1], argv[2], key, argc == 4 ? argv[3] : "qwen-plus");
        std::cout << "[PASS] live C++ RAG + stdio Tool answer chain\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] " << e.what() << "\n";
        return 1;
    }
}
