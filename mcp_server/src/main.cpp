//  The MIT License
//
//  Copyright (C) 2025 Giuseppe Mastrangelo
//
//  Permission is hereby granted, free of charge, to any person obtaining
//  a copy of this software and associated documentation files (the
//  'Software'), to deal in the Software without restriction, including
//  without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to
//  permit persons to whom the Software is furnished to do so, subject to
//  the following conditions:
//
//  The above copyright notice and this permission notice shall be
//  included in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//

#include "version.h"
#include "httplib.h"
#include "popl.hpp"
#include "StdioTransport.h"
#include "StreamableHttpTransport.h"
#include "Server.h"
#include "aixlog.hpp"
#include "PluginsLoader.h"
#include "json.hpp"
#include "MCPBuilder.h"
#include <csignal>

using namespace popl;

std::shared_ptr<vx::mcp::Server> server; // server实例

struct NotificationState {
    std::mutex serverNotificationMutex;
};
NotificationState notificationState;

/// Ctrl+C 停止处理函数
void stop_handler(sig_atomic_t s) {
    std::cout <<"Stopping server..." << std::endl;
    if (server && server->IsValid()) {
        server->Stop();
    }
    std::cout << "done." << std::endl;
    exit(0);
}

/// 插件到 mcp-client 的通知转发实现
void ClientNotificationCallbackImpl(const char* pluginName, const char* notification) {
    std::lock_guard<std::mutex> lock(notificationState.serverNotificationMutex);
    if (server && server->IsValid()) {
        server->SendNotification(pluginName, notification);
    }
}

/// 主入口
int main(int argc, char **argv) {
    std::string name;
    std::string plugins_directory;
    std::string logs_directory;
    bool verbose;

    // 三大核心
    std::shared_ptr<vx::ITransport> transport;
    auto loader = std::make_shared<vx::mcp::PluginsLoader>();
    server = std::make_shared<vx::mcp::Server>();

    //============================================================================================
    // 设置信号处理函数（Ctrl+C）
    //============================================================================================
    signal(SIGINT, stop_handler);

    //============================================================================================
    // 解析启动参数
    // 比如以stdio模式启动服务器，命令行为：./build/mcp_server/mcp_server -p ./build/mcp_server/plugins
    //============================================================================================
    OptionParser op("Allowed options");
    auto help_option = op.add<Switch>("", "help", "produce help message");
    auto name_option = op.add<Value<std::string>>("n", "name", "the name of the server", "mcp-server");
    auto plugins_directory_option = op.add<Value<std::string>>("p", "plugins", "the directory where to load the plugins", "./plugins");
    auto logs_directory_option = op.add<Value<std::string>>("l", "logs", "the directory where to store the logs", "./logs");
    auto verbose_option = op.add<Value<bool>>("v", "verbose", "enable verbose", verbose);
    auto use_streamable_http_server = op.add<Switch>("", "streamable-http", "start as Streamable HTTP server");
    name_option->assign_to(&name);
    plugins_directory_option->assign_to(&plugins_directory);
    logs_directory_option->assign_to(&logs_directory);
    verbose_option->assign_to(&verbose);

    //============================================================================================
    // 解析命令行选项
    //============================================================================================
    try {
        op.parse(argc, argv);
        if (help_option->count() == 1) {
            std::cout << op << std::endl;
            return 0;
        }
    } catch (const popl::invalid_option& e) {
        std::cerr << "Invalid Option Exception: " << e.what() << std::endl;
        return -1;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return -1;
    }

    //============================================================================================
    // 选择传输方式
    //============================================================================================
    if (use_streamable_http_server->is_set()) {
        transport = std::make_shared<vx::transport::StreamableHttp>();
    } else {
        transport = std::make_shared<vx::transport::Stdio>();
    }

    //============================================================================================
    // 初始化日志
    //============================================================================================
    // 获取当前时间并格式化为 ISO 8601 字符串
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t_now), "%Y-%m-%dT%H-%M-%S");
    std::string iso_date = ss.str();

    // 将 ISO 日期拼接到日志文件名中
    std::string logFilename = logs_directory + "/mcp-server_" + iso_date + ".log";
    auto sink_file = std::make_shared<AixLog::SinkFile>(AixLog::Severity::trace, logFilename);
    AixLog::Log::init({sink_file});

    //============================================================================================
    // 打印 logo 和启动信息
    //============================================================================================
    LOG(INFO) << " __  __  _____ _____        _____ ______ _______      ________ _____  " << std::endl;
    LOG(INFO) << "|  \\/  |/ ____|  __ \\      / ____|  ____|  __ \\ \\    / /  ____|  __ \\ " << std::endl;
    LOG(INFO) << "| \\  / | |    | |__) |____| (___ | |__  | |__) \\ \\  / /| |__  | |__) |" << std::endl;
    LOG(INFO) << "| |\\/| | |    |  ___/______\\___ \\|  __| |  _  / \\ \\/ / |  __| |  _  / " << std::endl;
    LOG(INFO) << "| |  | | |____| |          ____) | |____| | \\ \\  \\  /  | |____| | \\ \\ " << std::endl;
    LOG(INFO) << "|_|  |_|\\_____|_|         |_____/|______|_|  \\_\\  \\/   |______|_|  \\_\\" << std::endl;
    LOG(INFO) << "Starting mcp-server v" << PROJECT_VERSION << " (transport: " << transport->GetName() << " v" << transport->GetVersion() << ") on port: " << transport->GetPort() << std::endl;
    LOG(INFO) << "Press Ctrl+C to exit." << std::endl;

    //============================================================================================
    // 加载插件
    //============================================================================================
    if (loader->LoadPlugins(plugins_directory)) {
        LOG(INFO) << "Successfully loaded plugins" << std::endl;
    }

    //============================================================================================
    // 给插件注入通知回调
    //============================================================================================
    for (auto& plugin : loader->GetPlugins()) {
        plugin.instance->notifications = new NotificationSystem();
        plugin.instance->notifications->SendToClient = ClientNotificationCallbackImpl;
    }

    //============================================================================================
    // 启动 server
    //============================================================================================
    server->Name(name);
    server->VerboseLevel(verbose ? 1 : 0);

    // 这里是调用了这个函数，第二个参数是个lambda表达式
    // 意思是：把 tools/list 这个 method 对应的处理逻辑，替换成这里传进去的 lambda 函数。
    // server.cpp中实现的Overridecallback，实际上是定义了一个框架,在main.cpp中才真正为这个框架里面去装东西
    server->OverrideCallback("tools/list", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["tools"] = json::array();

        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_TOOLS) {
                for (int i = 0; i < plugin.instance->GetToolCount(); i++) {
                    nlohmann::ordered_json tool;
                    auto pluginTool = plugin.instance->GetTool(i);
                    tool["name"] = pluginTool->name;
                    tool["description"] = pluginTool->description;
                    tool["inputSchema"] = nlohmann::json::parse(pluginTool->inputSchema);
                    response["result"]["tools"].push_back(tool);
                }
            }
        }

        return response;
    });

    server->OverrideCallback("tools/call", [&loader](const json& request) {
        // 构建基础响应（填充 id / jsonrpc 等字段）
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        // 插件返回的原始字符串指针，由插件内部 malloc/new[] 分配
        char* res_ptr = nullptr;

        // 遍历所有已加载的插件，查找匹配的工具
        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_TOOLS) {
                for (int i = 0; i < plugin.instance->GetToolCount(); i++) {
                    auto pluginTool = plugin.instance->GetTool(i);
                    // 按工具名匹配请求中指定的工具
                    if (pluginTool->name == request["params"]["name"]) {
                        // 将请求序列化后交给插件处理
                        res_ptr = plugin.instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            // 尝试将插件返回的字符串解析为 JSON
                            try {
                                response["result"] = json::parse(res_ptr);
                                response["result"]["isError"] = false;
                            } catch (const json::parse_error& e) {
                                // 解析失败：返回错误提示
                                response["result"]["isError"] = true;
                                response["result"]["content"] = json::array();
                                response["result"]["content"].push_back({{"type", "text"}, {"text", "Plugin returned malformed data."}});
                            }
                            // 释放插件分配的 C 字符串内存
                            delete[] res_ptr;
                        } else {
                            LOG(ERROR) << "Plugin " << pluginTool->name << " returned nullptr." << std::endl;
                        }
                        return response;
                    }
                }
            }
        }

        // 未找到匹配的工具，返回错误
        response["result"]["isError"] = true;
        response["result"]["content"] = json::array();
        response["result"]["content"].push_back({
            {"type", "text"},
            {"text", "Tool not found: " + request["params"]["name"].get<std::string>()}
        });
        return response;
    });

    server->OverrideCallback("prompts/list", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["prompts"] = json::array();

        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_PROMPTS) {
                for (int i = 0; i < plugin.instance->GetPromptCount(); i++) {
                    nlohmann::ordered_json prompt;
                    auto pluginPrompt = plugin.instance->GetPrompt(i);
                    prompt["name"] = pluginPrompt->name;
                    prompt["description"] = pluginPrompt->description;
                    prompt["arguments"] = nlohmann::json::parse(pluginPrompt->arguments);
                    response["result"]["prompts"].push_back(prompt);
                }
            }
        }

        return response;
    });

    server->OverrideCallback("prompts/get", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        char* res_ptr = nullptr;

        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_PROMPTS) {
                for (int i = 0; i < plugin.instance->GetPromptCount(); i++) {
                    auto pluginPrompt = plugin.instance->GetPrompt(i);
                    if (pluginPrompt->name == request["params"]["name"]) {
                        res_ptr = plugin.instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            try {
                                response["result"] = json::parse(res_ptr);
                            } catch (const json::parse_error& e) {
                                LOG(ERROR) << "Plugin " << pluginPrompt->name << " returned malformed data." << std::endl;
                                // TODO: 这里应该如何处理错误？
                            }
                            // --- 释放已分配内存 ---
                            delete[] res_ptr;
                        }
                    }
                    return response;
                }
            }
        }

        return response;
    });

    server->OverrideCallback("resources/list", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);
        response["result"]["resources"] = json::array();

        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_RESOURCES) {
                for (int i = 0; i < plugin.instance->GetResourceCount(); i++) {
                    nlohmann::ordered_json resource;
                    auto pluginResource = plugin.instance->GetResource(i);
                    resource["name"] = pluginResource->name;
                    resource["description"] = pluginResource->description;
                    resource["uri"] = pluginResource->uri;
                    resource["mimeType"] = pluginResource->mime;
                    response["result"]["resources"].push_back(resource);
                }
            }
        }

        return response;
    });
    
    server->OverrideCallback("resources/read", [&loader](const json& request) {
        nlohmann::ordered_json response = MCPBuilder::Response(request);

        char* res_ptr = nullptr;

        for (const auto& plugin : loader->GetPlugins()) {
            if (plugin.instance->GetType() == PLUGIN_TYPE_RESOURCES) {
                for (int i = 0; i < plugin.instance->GetResourceCount(); i++) {
                    auto pluginResource = plugin.instance->GetResource(i);
                    if (pluginResource->uri == request["params"]["uri"]) {
                        res_ptr = plugin.instance->HandleRequest(request.dump().c_str());
                        if (res_ptr) {
                            try {
                                response["result"] = json::parse(res_ptr);
                            } catch (const json::parse_error& e) {
                                LOG(ERROR) << "Plugin " << pluginResource->name << " returned malformed data." << std::endl;
                                // TODO: 这里应该如何处理错误？
                            }
                            // --- 释放已分配内存 ---
                            delete[] res_ptr;
                        }
                    }
                }
            }
        }

        return response;
    });

    server->Connect(transport);

    return 0;
}
