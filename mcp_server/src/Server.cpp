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

#include <iostream>
#include <utility>
#include "Server.h"
#include "aixlog.hpp"
#include "version.h"
#include "MCPBuilder.h"

namespace vx::mcp {

    Server::Server() {
        functionMap = {
                {"initialize", [this](const json& req) { return this->InitializeCmd(req); }},
                {"ping", [this](const json& req) { return this->PingCmd(req); }},
                {"resources/list", [this](const json& req) { return this->ResourcesListCmd(req); }},
                {"resources/read", [this](const json& req) { return this->ResourcesReadCmd(req); }},
                {"tools/list", [this](const json& req) { return this->ToolsListCmd(req); }},
                {"tools/call", [this](const json& req) { return this->ToolsCallCmd(req); }},
                {"resources/subscribe", [this](const json& req) { return this->ResourcesSubscribeCmd(req); }},
                {"resources/unsubscribe", [this](const json& req) { return this->ResourcesUnsubscribeCmd(req); }},
                {"prompts/list", [this](const json& req) { return this->PromptsListCmd(req); }},
                {"prompts/get", [this](const json& req) { return this->PromptsGetCmd(req); }},
                {"logging/setLevel", [this](const json& req) { return this->LoggingSetLevelCmd(req); }},
                {"completion/complete", [this](const json& req) { return this->CompletionCompleteCmd(req); }},
                {"roots/list", [this](const json& req) { return this->RootsListCmd(req); }},
                {"notifications/initialized", [this](const json& req) { return this->NotificationInitializedCmd(req); }},
                {"notifications/cancelled", [this](const json& req) { return this->NotificationCancelledCmd(req); }},
                {"notifications/progress", [this](const json& req) { return this->NotificationProgressCmd(req); }},
                {"notifications/roots/list_changed", [this](const json& req) { return this->NotificationRootsListChangedCmd(req); }},
                {"notifications/resources/list_changed", [this](const json& req) { return this->NotificationResourcesListChangedCmd(req); }},
                {"notifications/resources/updated", [this](const json& req) { return this->NotificationResourcesUpdatedCmd(req); }},
                {"notifications/prompts/list_changed", [this](const json& req) { return this->NotificationPromptsListChangedCmd(req); }},
                {"notifications/tools/list_changed", [this](const json& req) { return this->NotificationToolsListChangedCmd(req); }},
                {"notifications/message", [this](const json& req) { return this->NotificationMessageCmd(req); }}
        };
    }

    Server::~Server() {
        Stop();
    }

    // 写线程，当队列为空时，该线程等待；当队列有通知时，将其写回给client
    void Server::WriterLoop() {
        LOG(INFO) << "Writer thread started." << std::endl;
        while (writer_running_.load()) {
            std::string notification_to_send;
            {
                std::unique_lock<std::mutex> lock(output_mutex_);
                // 等待队列非空，或写线程需要停止
                queue_cv_.wait(lock, [this] { return !notification_queue_.empty() || !writer_running_.load(); });

                // 唤醒后再次检查运行标志
                if (!writer_running_.load() && notification_queue_.empty()) {
                    break; // 如果已经停止且队列为空，则退出循环
                }

                if (!notification_queue_.empty()) {
                    notification_to_send = std::move(notification_queue_.front());
                    notification_queue_.pop();
                }
            } // 在可能阻塞的写操作前释放锁

            if (!notification_to_send.empty() && transport_) {
                try {
                    // 这里重新加锁，避免通知写入和响应写入并发交叉
                    std::lock_guard<std::mutex> write_lock(output_mutex_);
                    if (transport_) { // 延迟后再次检查 transport 是否仍然有效
                        LOG(DEBUG) << "Sending Notification: " << notification_to_send << std::endl;
                        transport_->Write(notification_to_send);
                    }
                } catch (const std::exception& e) {
                    LOG(ERROR) << "Error writing notification: " << e.what() << std::endl;
                    // 写入失败时目前只记录日志，后续可按需要决定是否停止服务
                }
            }
            // 短暂休眠，避免错误频繁发生时进入紧密循环
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        LOG(INFO) << "Writer thread stopped." << std::endl;
    }

    bool Server::Connect(const std::shared_ptr<ITransport> &transport) {
        if (!transport) {
            LOG(ERROR) << "Connect called with null transport." << std::endl;
            return false;
        }

        transport_ = transport; // 保存传输层指针
        isStopping_ = false;
        request_worker_running_ = true;
        request_thread_ = std::thread(&Server::RequestLoop, this); // 重置停止标志

        // 启动写指针
        writer_running_ = true;
        writer_thread_ = std::thread(&Server::WriterLoop, this);

        // 启动传输层；HTTP 传输需要真正启动服务，stdio 通常直接返回 true
        if (!transport_->Start()) {
            LOG(ERROR) << "Failed to start transport: " << transport_->GetName() << std::endl;
            Stop();
            return false;
        }

        // 循环读取请求
        while (!isStopping_) {
            auto [length, json_string] = transport->Read();
            if (isStopping_) break;

            if (length == 0 && json_string.empty()) {
                LOG(INFO) << "Read returned empty data, potentially client disconnected." << std::endl;
                isStopping_ = true;
                break;
            }

            try {
                // 解析请求json
                if (json_string.empty()) continue;
                LOG(DEBUG) << "Received: " << json_string << std::endl;
                json request = json::parse(json_string);
                parserErrors_ = 0; // 重置解析错误计数

                DispatchRequest(request);
            } catch (json::parse_error &e) {
                // 解析失败时暂不退出进程，只限制连续解析错误次数
                LOG(ERROR) << "Error parsing JSON: " << e.what() << std::endl;
                if (++parserErrors_ > MAX_PARSER_ERRORS) return false;
            }
        }

        StopRequestWorker(false); // Drain replies after a stdio half-close.
        Stop();

        return true;
    }

    bool Server::ConnectAsync(const std::shared_ptr<ITransport> &transport) {
        if (!transport) {
            LOG(ERROR) << "ConnectAsync called with null transport." << std::endl;
            return false;
        }

        transport_ = transport;
        isStopping_ = false;
        request_worker_running_ = true;
        request_thread_ = std::thread(&Server::RequestLoop, this);

        // 启动写线程
        writer_running_ = true;
        writer_thread_ = std::thread(&Server::WriterLoop, this);

        if (!transport_->Start()) { Stop(); return false; }

        // 启动异步读线程
        reader_running_ = true;
        reader_thread_ = std::thread([this]() {
            LOG(INFO) << "Async Reader thread started." << std::endl;
            while (reader_running_ && !isStopping_) {
                try {
                    auto future = transport_->ReadAsync();
                    auto [length, json_string] = future.get();

                    if (isStopping_ || (length == 0 && json_string.empty())) {
                        LOG(INFO) << "Empty message or stopping. Reader exiting.";
                        break;
                    }

                    if (!json_string.empty()) {
                        LOG(DEBUG) << "Received: " << json_string << std::endl;
                        json request = json::parse(json_string);
                        parserErrors_ = 0;

                        DispatchRequest(request);
                    }
                } catch (json::parse_error &e) {
                    LOG(ERROR) << "Error parsing JSON: " << e.what() << std::endl;
                    if (++parserErrors_ > MAX_PARSER_ERRORS) {
                        isStopping_ = true;
                        break;
                    }
                } catch (const std::exception &e) {
                    LOG(ERROR) << "Reader thread exception: " << e.what() << std::endl;
                    isStopping_ = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            LOG(INFO) << "Async Reader thread exiting." << std::endl;
        });

        return true;
    }

    void Server::Stop() {
        isStopping_ = true;
        StopRequestWorker(true);
        writer_running_ = false;
        queue_cv_.notify_all();
        if (writer_thread_.joinable()) writer_thread_.join();
        reader_running_ = false;
        if (transport_) transport_->Stop();
        if (reader_thread_.joinable() && reader_thread_.get_id() != std::this_thread::get_id()) reader_thread_.join();
        transport_.reset();
    }

    void Server::DispatchRequest(const json& request) {
        if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
            std::lock_guard<std::mutex> output_lock(output_mutex_);
            transport_->Write(MCPBuilder::Error(MCPBuilder::InvalidRequest,
                request.is_object() ? request.value("id", json(nullptr)) : json(nullptr), "Invalid request").dump());
            return;
        }
        const std::string method = request["method"].get<std::string>();
        const bool has_id = request.is_object() && request.contains("id") && !request["id"].is_null();
        if (has_id && (method == "tools/call" || method == "prompts/get" || method == "resources/read")) {
            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            const std::string key = request["id"].dump();
            {
                std::lock_guard<std::mutex> lock(request_mutex_);
                if (request_cancellations_.count(key) || request_queue_.size() >= 256) {
                    std::lock_guard<std::mutex> output_lock(output_mutex_);
                    transport_->Write(MCPBuilder::Error(MCPBuilder::InvalidRequest, request["id"],
                        "Duplicate request ID or server busy").dump());
                    return;
                }
                request_cancellations_[key] = cancelled;
                request_queue_.push({request, cancelled});
            }
            request_cv_.notify_one();
            return;
        }
        json response = HandleRequest(request);
        if (has_id && !response.is_null()) {
            std::lock_guard<std::mutex> lock(output_mutex_);
            transport_->Write(response.dump());
        }
    }

    void Server::RequestLoop() {
        for (;;) {
            PendingRequest pending;
            {
                std::unique_lock<std::mutex> lock(request_mutex_);
                request_cv_.wait(lock, [this] { return !request_worker_running_ || !request_queue_.empty(); });
                if (request_queue_.empty() && !request_worker_running_) return;
                pending = std::move(request_queue_.front());
                request_queue_.pop();
            }
            json response = nullptr;
            if (!pending.cancelled->load()) {
                try {
                    response = HandleRequest(pending.request);
                } catch (const std::exception& error) {
                    response = MCPBuilder::Error(MCPBuilder::InternalError, pending.request["id"], error.what());
                }
            }
            // Serialize cancellation with response delivery; a cancelled job never publishes a late reply.
            std::lock_guard<std::mutex> lock(request_mutex_);
            if (!pending.cancelled->load() && !response.is_null()) {
                std::lock_guard<std::mutex> output_lock(output_mutex_);
                transport_->Write(response.dump());
            }
            request_cancellations_.erase(pending.request["id"].dump());
        }
    }

    void Server::StopRequestWorker(bool cancel_pending) {
        {
            std::lock_guard<std::mutex> lock(request_mutex_);
            request_worker_running_ = false;
            if (cancel_pending) {
                for (auto& [id, cancelled] : request_cancellations_) cancelled->store(true);
            }
        }
        request_cv_.notify_all();
        if (request_thread_.joinable()) request_thread_.join();
    }

    std::shared_ptr<std::atomic<bool>> Server::GetCancellationToken(const json& request_id) const {
        std::lock_guard<std::mutex> lock(request_mutex_);
        auto it = request_cancellations_.find(request_id.dump());
        return it == request_cancellations_.end() ? nullptr : it->second;
    }

    void Server::SendNotification(const std::string& pluginName, const char* notification) {
        if (isStopping_) {
            LOG(WARNING) << pluginName << " attempted to send notification while server stopping." << std::endl;
            return;
        }

        // 将通知加入队列，并用互斥量保护
        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            notification_queue_.emplace(notification);
        }
        queue_cv_.notify_one(); // 唤醒写线程
    }

    json Server::HandleRequest(const json &request) {
        // 记录请求
        if (verboseLevel_ == 1) {
            LOG(DEBUG) << "=== Request START ===" << std::endl;
            LOG(DEBUG) << request.dump(4) << std::endl;
            LOG(DEBUG) << "=== Request END ===" << std::endl;
        }

        // 安全获取 id（JSON-RPC notification 不含 "id" 字段）
        const bool has_id = request.contains("id");
        const json request_id = has_id ? request["id"] : json(nullptr);

        // 检查request中是否有method字段
        if (!request.contains("method")) {
            if (has_id) {
                return MCPBuilder::Error(MCPBuilder::InvalidRequest, request_id, "Missing method");
            }
            return nullptr; // 无 method 也无 id 的通知，静默忽略
        }

        // 在 functionMap 里找 handler
        std::string methodName = request["method"];
        auto it = functionMap.find(methodName);
        if (it != functionMap.end()) {
            json response = it->second(request);
            if (response != nullptr) {
                if (verboseLevel_ == 1) {
                    LOG(DEBUG) << "=== Response START ===" << std::endl;
                    LOG(DEBUG) << response.dump(4) << std::endl;
                    LOG(DEBUG) << "=== Response END ===" << std::endl;
                }
            }
            return response;
        }

        // 处理找不到 method 的情况
        if (has_id) {
            return MCPBuilder::Error(MCPBuilder::MethodNotFound, request_id, "Method not found");
        }
        // 未注册的通知（notification），按 JSON-RPC 规范静默忽略
        return nullptr;
    }

    bool Server::OverrideCallback(const std::string &method, std::function<json(const json &)> function) {
        // 在路由表中找到该方法（我们定义过）
        if (functionMap.find(method) != functionMap.end()) {
            functionMap[method] = std::move(function);
            return true;
        }
        return false;
    }

    // 处理 mcp initialize 请求：解析客户端参数并返回服务端能力声明
    json Server::InitializeCmd(const json &request) {
        LOG(INFO) << "InitializeCommand" << std::endl;
        if (request.contains("params")) {
            json params = request["params"];

    // 解析 client 初始化参数，并记录一些调试信息。
            // 工作区根路径
            if (params.contains("rootUri")) {
                std::string rootUri = params["rootUri"].get<std::string>();
                LOG(INFO) << "rootUri: " << rootUri << std::endl;
            }

            // rootPath（已废弃，保留兼容）
            if (params.contains("rootPath")) {
                std::string rootPath = params["rootPath"].get<std::string>();
                LOG(INFO) << "rootPath: " << rootPath << std::endl;
            }

            // 初始化选项，可携带自定义配置
            if (params.contains("initializationOptions")) {
                json initializationOptions = params["initializationOptions"];
                LOG(INFO) << "initializationOptions: " << initializationOptions.dump() << std::endl;
            }

            // 客户端能力集：工作区、文档同步、补全等
            if (params.contains("capabilities")) {
                json capabilities = params["capabilities"];

                // 是否支持多工作区文件夹
                if (capabilities.contains("workspace") && capabilities["workspace"].contains("workspaceFolders")) {
                    bool workspaceFolders = capabilities["workspace"]["workspaceFolders"].get<bool>();
                    LOG(INFO) << "workspaceFolders: " << workspaceFolders << std::endl;
                }

                // 文档同步方式（全量 / 增量）
                if (capabilities.contains("textDocument") && capabilities["textDocument"].contains("synchronization")) {
                    json synchronization = capabilities["textDocument"]["synchronization"];
                    if (synchronization.contains("didChange") && synchronization["didChange"].contains("synchronizationKind")){
                        int synchronizationKind = synchronization["didChange"]["synchronizationKind"].get<int>();
                        LOG(INFO) << "synchronizationKind: " << synchronizationKind << std::endl;
                    }
                }

                // 补全项能力（如是否支持 snippet）
                if (capabilities.contains("textDocument") && capabilities["textDocument"].contains("completion") && capabilities["textDocument"]["completion"].contains("completionItem")) {
                    json completionItem = capabilities["textDocument"]["completion"]["completionItem"];
                    if (completionItem.contains("snippetSupport")){
                        bool snippetSupport = completionItem["snippetSupport"].get<bool>();
                        LOG(INFO) << "snippetSupport: " << snippetSupport << std::endl;
                    }
                }
            }

            // 日志追踪级别
            if (params.contains("trace")) {
                std::string trace = params["trace"].get<std::string>();
                LOG(INFO) << "trace: " << trace << std::endl;
            }

            // 工作区文件夹列表
            if (params.contains("workspaceFolders")) {
                json workspaceFoldersArray = params["workspaceFolders"];
                for (const auto& folder : workspaceFoldersArray) {
                    std::string uri = folder["uri"].get<std::string>();
                    std::string name = folder["name"].get<std::string>();
                    LOG(INFO) << "workspaceFolder uri: " << uri << " name: " << name << std::endl;
                }
            }
        }

    // 构造 initialize 响应json，声明服务端提供的能力
        nlohmann::ordered_json response = {};
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", json(nullptr));
        response["result"]["protocolVersion"] = request.contains("params")
            ? request["params"].value("protocolVersion", "2024-11-05")
            : "2024-11-05";
        /// TODO: "listChanged" 参数会导致 Claude Desktop 崩溃
        /// 是否需要根据协议版本号做判断？
        response["result"]["capabilities"]["tools"] = json::object();
        response["result"]["capabilities"]["prompts"] = json::object();
        response["result"]["capabilities"]["resources"]["subscribe"] = true;
        response["result"]["capabilities"]["logging"] = json::object();
        response["result"]["serverInfo"]["name"] = name_;
        response["result"]["serverInfo"]["version"] = PROJECT_VERSION;
        return response;
    }

    json Server::PingCmd(const json &request) {
        nlohmann::ordered_json response = {};
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", json(nullptr));
        response["result"] = json::object();
        return response;
    }

    json Server::ResourcesListCmd(const json &request) {
        nlohmann::ordered_json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", json(nullptr));
        response["result"]["resources"] = json::array();
        return response;
    }

    json Server::ResourcesReadCmd(const json &request) {
        return json();
    }

    json Server::ToolsListCmd(const json &request) {
        nlohmann::ordered_json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", json(nullptr));
        response["result"]["tools"] = json::array();
        return response;
    }

    json Server::ToolsCallCmd(const json &request) {
        LOG(DEBUG) << "ToolsCallCmd called" << std::endl;
        nlohmann::ordered_json response;
        nlohmann::ordered_json defaultTextContent;

        defaultTextContent["type"] = "text";
        defaultTextContent["text"] = "you should override this method in your plugin.";

        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", json(nullptr));
        response["result"]["content"] = json::array();
        response["result"]["content"].push_back(defaultTextContent);
        response["result"]["isError"] = true;

        return response;
    }

    json Server::ResourcesSubscribeCmd(const json &request) {
        LOG(WARNING) << "ResourcesSubscribeCmd called but NOT YET IMPLEMENTED" << std::endl;
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request.value("id", json(nullptr)), "Method not found");
    }

    json Server::ResourcesUnsubscribeCmd(const json &request) {
        LOG(WARNING) << "ResourcesUnsubscribeCmd called but NOT YET IMPLEMENTED" << std::endl;
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request.value("id", json(nullptr)), "Method not found");
    }

    json Server::PromptsListCmd(const json &request) {
        nlohmann::ordered_json response;
        response["jsonrpc"] = "2.0";
        response["id"] = request.value("id", json(nullptr));
        response["result"]["prompts"] = json::array();
        return response;
    }

    json Server::PromptsGetCmd(const json &request) {
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request.value("id", json(nullptr)), "Method not found");
    }

    json Server::LoggingSetLevelCmd(const json &request) {
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request.value("id", json(nullptr)), "Method not found");
    }

    json Server::CompletionCompleteCmd(const json &request) {
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request.value("id", json(nullptr)), "Method not found");
    }

    json Server::RootsListCmd(const json &request) {
        return MCPBuilder::Error(MCPBuilder::MethodNotFound, request.value("id", json(nullptr)), "Method not found");
    }

    json Server::NotificationInitializedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationCancelledCmd(const json &request) {
        if (!request.contains("params") || !request["params"].is_object() ||
            !request["params"].contains("requestId")) return nullptr;
        const auto& id = request["params"]["requestId"];
        if (!id.is_string() && !id.is_number_integer()) return nullptr;
        const std::string key = id.dump();
        {
            std::lock_guard<std::mutex> lock(request_mutex_);
            auto it = request_cancellations_.find(key);
            if (it == request_cancellations_.end()) return nullptr;
            it->second->store(true);
        }
        transport_->CancelRequest(key);
        return nullptr;
    }

    json Server::NotificationProgressCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationRootsListChangedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationResourcesListChangedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationResourcesUpdatedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationPromptsListChangedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationToolsListChangedCmd(const json &request) {
        return nullptr;
    }

    json Server::NotificationMessageCmd(const json &request) {
        return nullptr;
    }

    void Server::StopAsync() {
        Stop();
    }
}
