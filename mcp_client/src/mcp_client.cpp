#include "agent_rpc/mcp/mcp_client.h"
#include "agent_rpc/common/logger.h"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sstream>
#include <fstream>
#include <chrono>
#include <json/json.h>
#include <ctime>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>

namespace agent_rpc {
namespace mcp {

// MCPClient 实现
MCPClient::MCPClient() {
    // Logger 使用全局宏，不需要实例化
}

MCPClient::~MCPClient() {
    disconnect();
}

bool MCPClient::connect(const std::string& server_path, const std::vector<std::string>& args) {
    // 使用 STDIO 模式连接
    MCPConnectionConfig config;
    config.transport = MCPTransportType::STDIO;
    config.server_path = server_path;
    config.server_args = args;
    return connect(config);
}

bool MCPClient::connect(const MCPConnectionConfig& config) {
    // 1. 避免重复连接
    if (connected_) {
        LOG_WARN("MCP client already connected");
        return true;
    }
    
    // 2. 保存连接配置和传输类型
    config_ = config;
    transport_type_ = config.transport;
    
    bool success = false;
    
    // 3. STDIO 模式：启动本地 Server 子进程并建立双向管道
    if (transport_type_ == MCPTransportType::STDIO) {
        server_path_ = config.server_path;
        server_args_ = config.server_args;
        
        if (!startMCPServer()) {
            LOG_ERROR("Failed to start MCP server");
            return false;
        }
        
        // 4. 标记 Client 已连接，并允许后台线程继续运行
        connected_ = true;
        running_ = true;
        
        // 5. 持续读取 Server stdout，处理响应和通知
        notification_thread_ = std::thread([this]() {
            processNotificationsStdio();
        });
        
        LOG_INFO("MCP client connected to server (STDIO): " + server_path_);
        success = true;
    } else if (transport_type_ == MCPTransportType::STREAMABLE_HTTP) {
        // 3. Streamable HTTP 模式：初始化 HTTP 客户端
        if (!connectStreamableHttp()) {
            LOG_ERROR("Failed to connect to MCP server via Streamable HTTP");
            return false;
        }
        
        // 4. 标记 Client 已连接，并允许后台线程继续运行
        connected_ = true;
        running_ = true;

        // 5. 已设置通知回调时，启动 Server 事件监听线程
        if (notification_callback_) {
            stream_event_thread_ = std::thread([this]() {
                processStreamableHttpEvents();
            });
        }

        LOG_INFO("MCP client connected to server (Streamable HTTP): " + config.mcp_url);
        success = true;
    }
    
    // 6. 未识别的传输类型会保持 false
    return success;
}

MCPTransportType MCPClient::getTransportType() const {
    return transport_type_;
}

void MCPClient::disconnect() {
    if (!connected_) {
        return;
    }
    
    running_ = false;
    connected_ = false;
    
    // 停止通知线程
    if (notification_thread_.joinable()) {
        notification_thread_.join();
    }
    
    if (transport_type_ == MCPTransportType::STDIO) {
        stopMCPServer();
    } else if (transport_type_ == MCPTransportType::STREAMABLE_HTTP) {
        disconnectStreamableHttp();
    }
    
    LOG_INFO("MCP client disconnected");
}

bool MCPClient::isConnected() const {
    return connected_;
}

std::vector<MCPTool> MCPClient::listTools() {
    std::vector<MCPTool> tools;
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return tools;
    }
    
    MCPRequest request;
    request.method = "tools/list";
    request.id = "list_tools_" + std::to_string(std::time(nullptr));
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send tools/list request");
        return tools;
    }
    
    MCPResponse response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error listing tools: " + response.error);
        return tools;
    }
    
    // 解析响应
    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(response.result, root)) {
            const Json::Value& tools_array = root["tools"];
            for (const auto& tool : tools_array) {
                MCPTool mcp_tool;
                mcp_tool.name = tool["name"].asString();
                mcp_tool.description = tool["description"].asString();
                mcp_tool.input_schema = tool["inputSchema"].toStyledString();
                tools.push_back(mcp_tool);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse tools list: " + std::string(e.what()));
    }
    
    return tools;
}

MCPResponse MCPClient::callTool(const std::string& tool_name, const std::string& arguments) {
    MCPResponse response;
    response.is_error = true;
    response.error = "Not connected";
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return response;
    }
    
    MCPRequest request;
    request.method = "tools/call";
    request.id = "call_tool_" + std::to_string(std::time(nullptr));
    
    // 构建参数
    Json::Value params;
    params["name"] = tool_name;
    
    Json::Value args_json;
    Json::Reader reader;
    if (reader.parse(arguments, args_json)) {
        params["arguments"] = args_json;
    } else {
        params["arguments"] = Json::Value(Json::objectValue);
    }
    
    // 只设置 params 部分，不要包含 method 和 id
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    request.params = Json::writeString(builder, params);
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send tools/call request");
        response.error = "Failed to send request";
        return response;
    }
    
    response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error calling tool " + tool_name + ": " + response.error);
    } else {
        LOG_INFO("Successfully called tool: " + tool_name);
    }
    
    return response;
}

std::vector<MCPPrompt> MCPClient::listPrompts() {
    std::vector<MCPPrompt> prompts;
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return prompts;
    }
    
    MCPRequest request;
    request.method = "prompts/list";
    request.id = "list_prompts_" + std::to_string(std::time(nullptr));
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send prompts/list request");
        return prompts;
    }
    
    MCPResponse response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error listing prompts: " + response.error);
        return prompts;
    }
    
    // 解析响应
    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(response.result, root)) {
            const Json::Value& prompts_array = root["prompts"];
            for (const auto& prompt : prompts_array) {
                MCPPrompt mcp_prompt;
                mcp_prompt.name = prompt["name"].asString();
                mcp_prompt.description = prompt["description"].asString();
                mcp_prompt.arguments = prompt["arguments"].toStyledString();
                prompts.push_back(mcp_prompt);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse prompts list: " + std::string(e.what()));
    }
    
    return prompts;
}

MCPResponse MCPClient::getPrompt(const std::string& prompt_name, const std::string& arguments) {
    MCPResponse response;
    response.is_error = true;
    response.error = "Not connected";
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return response;
    }
    
    MCPRequest request;
    request.method = "prompts/get";
    request.id = "get_prompt_" + std::to_string(std::time(nullptr));
    
    // 构建参数
    Json::Value params;
    params["name"] = prompt_name;
    
    Json::Value args_json;
    Json::Reader reader;
    if (reader.parse(arguments, args_json)) {
        params["arguments"] = args_json;
    } else {
        params["arguments"] = Json::Value();
    }
    
    Json::Value request_obj;
    request_obj["method"] = request.method;
    request_obj["params"] = params;
    request_obj["id"] = request.id;
    
    request.params = request_obj.toStyledString();
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send prompts/get request");
        response.error = "Failed to send request";
        return response;
    }
    
    response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error getting prompt " + prompt_name + ": " + response.error);
    } else {
        LOG_INFO("Successfully got prompt: " + prompt_name);
    }
    
    return response;
}

std::vector<MCPResource> MCPClient::listResources() {
    std::vector<MCPResource> resources;
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return resources;
    }
    
    MCPRequest request;
    request.method = "resources/list";
    request.id = "list_resources_" + std::to_string(std::time(nullptr));
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send resources/list request");
        return resources;
    }
    
    MCPResponse response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error listing resources: " + response.error);
        return resources;
    }
    
    // 解析响应
    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(response.result, root)) {
            const Json::Value& resources_array = root["resources"];
            for (const auto& resource : resources_array) {
                MCPResource mcp_resource;
                mcp_resource.name = resource["name"].asString();
                mcp_resource.description = resource["description"].asString();
                mcp_resource.uri = resource["uri"].asString();
                mcp_resource.mime_type = resource["mimeType"].asString();
                resources.push_back(mcp_resource);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse resources list: " + std::string(e.what()));
    }
    
    return resources;
}

MCPResponse MCPClient::readResource(const std::string& uri) {
    MCPResponse response;
    response.is_error = true;
    response.error = "Not connected";
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return response;
    }
    
    MCPRequest request;
    request.method = "resources/read";
    request.id = "read_resource_" + std::to_string(std::time(nullptr));
    
    // 构建参数
    Json::Value params;
    params["uri"] = uri;
    
    Json::Value request_obj;
    request_obj["method"] = request.method;
    request_obj["params"] = params;
    request_obj["id"] = request.id;
    
    request.params = request_obj.toStyledString();
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send resources/read request");
        response.error = "Failed to send request";
        return response;
    }
    
    response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error reading resource " + uri + ": " + response.error);
    } else {
        LOG_INFO("Successfully read resource: " + uri);
    }
    
    return response;
}

void MCPClient::setNotificationCallback(std::function<void(const std::string&, const std::string&)> callback) {
    notification_callback_ = callback;
    if (transport_type_ == MCPTransportType::STREAMABLE_HTTP &&
        connected_ &&
        notification_callback_ &&
        !stream_event_thread_.joinable()) {
        stream_event_thread_ = std::thread([this]() {
            processStreamableHttpEvents();
        });
    }
}

bool MCPClient::sendRequest(const MCPRequest& request) {
    if (transport_type_ == MCPTransportType::STDIO) {
        return sendRequestStdio(request);
    } else if (transport_type_ == MCPTransportType::STREAMABLE_HTTP) {
        return sendRequestStreamableHttp(request);
    }
    return false;
}

bool MCPClient::sendRequestStdio(const MCPRequest& request) {
    if (stdin_pipe_ == -1) {
        LOG_ERROR("MCP server stdin pipe not available");
        return false;
    }
    
    std::string json_request = buildJSONRPCRequest(request);
    json_request += "\n";  // MCP协议使用换行符分隔消息
    
    ssize_t written = write(stdin_pipe_, json_request.c_str(), json_request.length());
    if (written != static_cast<ssize_t>(json_request.length())) {
        LOG_ERROR("Failed to write request to MCP server");
        return false;
    }
    
    return true;
}

MCPResponse MCPClient::receiveResponse() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    
    // 等待响应，最多等待30秒
    if (queue_cv_.wait_for(lock, std::chrono::seconds(30), [this] { return !response_queue_.empty(); })) {
        MCPResponse response = response_queue_.front();
        response_queue_.pop();
        return response;
    }
    
    MCPResponse timeout_response;
    timeout_response.is_error = true;
    timeout_response.error = "Request timeout";
    return timeout_response;
}

void MCPClient::processNotificationsStdio() {
    if (stdout_pipe_ == -1) {
        LOG_ERROR("MCP server stdout pipe not available");
        return;
    }
    
    char buffer[4096];
    std::string line;
    
    while (running_) {
        ssize_t bytes_read = read(stdout_pipe_, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            line += buffer;
            
            // 处理完整的行
            size_t pos = 0;
            while ((pos = line.find('\n')) != std::string::npos) {
                std::string message = line.substr(0, pos);
                line.erase(0, pos + 1);
                
                if (!message.empty()) {
                    MCPResponse response = parseJSONRPCResponse(message);
                    
                    // 检查是否是通知
                    if (response.id.empty() && !response.error.empty()) {
                        // 这是一个通知
                        if (notification_callback_) {
                            // 解析通知内容
                            try {
                                Json::Value root;
                                Json::Reader reader;
                                if (reader.parse(message, root)) {
                                    std::string method = root["method"].asString();
                                    if (method == "notifications/message") {
                                        const Json::Value& params = root["params"];
                                        std::string plugin_name = params["pluginName"].asString();
                                        std::string notification = params["notification"].asString();
                                        notification_callback_(plugin_name, notification);
                                    }
                                }
                            } catch (const std::exception& e) {
                                LOG_WARN("Failed to parse notification: " + std::string(e.what()));
                            }
                        }
                    } else {
                        // 这是一个响应
                        std::lock_guard<std::mutex> lock(queue_mutex_);
                        response_queue_.push(response);
                        queue_cv_.notify_one();
                    }
                }
            }
        } else if (bytes_read == 0) {
            // 管道关闭
            break;
        } else {
            // 读取错误
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Error reading from MCP server stdout");
                break;
            }
            // 非阻塞管道暂无数据，休眠避免 busy-wait 占满 CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

bool MCPClient::startMCPServer() {
    // 创建管道
    int stdin_pipe_fd[2];
    int stdout_pipe_fd[2];
    
    if (pipe(stdin_pipe_fd) == -1 || pipe(stdout_pipe_fd) == -1) {
        LOG_ERROR("Failed to create pipes for MCP server");
        return false;
    }
    
    // 创建子进程
    server_pid_ = fork();
    if (server_pid_ == -1) {
        LOG_ERROR("Failed to fork process for MCP server");
        return false;
    }
    
    if (server_pid_ == 0) {
        // 子进程：运行MCP服务器
        close(stdin_pipe_fd[1]);  // 关闭写端
        close(stdout_pipe_fd[0]); // 关闭读端
        
        // 重定向stdin和stdout
        dup2(stdin_pipe_fd[0], STDIN_FILENO);
        dup2(stdout_pipe_fd[1], STDOUT_FILENO);
        
        // 准备参数
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(server_path_.c_str()));
        
        for (const auto& arg : server_args_) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        
        // 执行MCP服务器
        execv(server_path_.c_str(), argv.data());
        
        // 如果execv失败
        LOG_ERROR("Failed to execute MCP server: " + server_path_);
        exit(1);
    } else {
        // 父进程
        close(stdin_pipe_fd[0]);  // 关闭读端
        close(stdout_pipe_fd[1]); // 关闭写端
        
        stdin_pipe_ = stdin_pipe_fd[1];
        stdout_pipe_ = stdout_pipe_fd[0];
        
        // 设置非阻塞模式
        fcntl(stdout_pipe_, F_SETFL, O_NONBLOCK);
        
        // 等待服务器启动
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        return true;
    }
}

void MCPClient::stopMCPServer() {
    if (server_pid_ > 0) {
        kill(server_pid_, SIGTERM);
        waitpid(server_pid_, nullptr, 0);
        server_pid_ = -1;
    }
    
    if (stdin_pipe_ != -1) {
        close(stdin_pipe_);
        stdin_pipe_ = -1;
    }
    
    if (stdout_pipe_ != -1) {
        close(stdout_pipe_);
        stdout_pipe_ = -1;
    }
}

std::string MCPClient::buildJSONRPCRequest(const MCPRequest& request) {
    Json::Value root;
    root["jsonrpc"] = "2.0";
    root["method"] = request.method;
    root["id"] = request.id;
    
    if (!request.params.empty()) {
        Json::Value params;
        Json::Reader reader;
        if (reader.parse(request.params, params)) {
            root["params"] = params;
        }
    }
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

MCPResponse MCPClient::parseJSONRPCResponse(const std::string& response) {
    MCPResponse mcp_response;
    
    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(response, root)) {
            mcp_response.id = root["id"].asString();
            
            if (root.isMember("error")) {
                mcp_response.is_error = true;
                mcp_response.error = root["error"]["message"].asString();
            } else {
                mcp_response.is_error = false;
                mcp_response.result = root["result"].toStyledString();
            }
        }
    } catch (const std::exception& e) {
        mcp_response.is_error = true;
        mcp_response.error = "Failed to parse response: " + std::string(e.what());
    }
    
    return mcp_response;
}

// ============================================================================
// Streamable HTTP 传输实现
// ============================================================================

bool MCPClient::connectStreamableHttp() {
    curl_global_init(CURL_GLOBAL_ALL);
    curl_handle_ = curl_easy_init();
    if (!curl_handle_) {
        LOG_ERROR("Failed to initialize CURL for Streamable HTTP");
        return false;
    }
    return true;
}

void MCPClient::disconnectStreamableHttp() {
    running_ = false;

    if (stream_event_thread_.joinable()) {
        stream_event_thread_.join();
    }

    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }

    curl_global_cleanup();
    session_id_.clear();
    stream_response_buffer_.clear();

    LOG_INFO("Streamable HTTP connection closed");
}

bool MCPClient::sendRequestStreamableHttp(const MCPRequest& request) {
    if (!curl_handle_) {
        LOG_ERROR("Streamable HTTP not connected");
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to create CURL handle for Streamable HTTP request");
        return false;
    }

    std::string endpoint = getMcpEndpoint();
    std::string response_data;
    std::string json_request = buildJSONRPCRequest(request);

    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_request.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_request.length());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.request_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config_.connect_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, streamableHttpHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            std::string* data = static_cast<std::string*>(userdata);
            data->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    headers = curl_slist_append(headers, "MCP-Protocol-Version: 2025-11-25");
    if (!session_id_.empty()) {
        std::string session_header = "MCP-Session-Id: " + session_id_;
        headers = curl_slist_append(headers, session_header.c_str());
    }
    if (!config_.api_key.empty()) {
        std::string auth_header = "Authorization: Bearer " + config_.api_key;
        headers = curl_slist_append(headers, auth_header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (!config_.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR("Streamable HTTP request failed: " + std::string(curl_easy_strerror(res)));
        return false;
    }
    if (http_code >= 400) {
        LOG_ERROR("Streamable HTTP request returned HTTP " + std::to_string(http_code) + ": " + response_data);
        return false;
    }

    if (!response_data.empty()) {
        MCPResponse response = parseJSONRPCResponse(response_data);
        std::lock_guard<std::mutex> lock(queue_mutex_);
        response_queue_.push(response);
        queue_cv_.notify_one();
    }

    return true;
}

void MCPClient::processStreamableHttpEvents() {
    while (running_) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            LOG_ERROR("Failed to create CURL handle for Streamable HTTP events");
            return;
        }

        std::string endpoint = getMcpEndpoint();
        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamableHttpEventCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, streamableHttpHeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config_.connect_timeout_ms));

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        headers = curl_slist_append(headers, "Cache-Control: no-cache");
        headers = curl_slist_append(headers, "MCP-Protocol-Version: 2025-11-25");
        if (!session_id_.empty()) {
            std::string session_header = "MCP-Session-Id: " + session_id_;
            headers = curl_slist_append(headers, session_header.c_str());
        }
        if (!config_.api_key.empty()) {
            std::string auth_header = "Authorization: Bearer " + config_.api_key;
            headers = curl_slist_append(headers, auth_header.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        if (!config_.verify_ssl) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK && res != CURLE_OPERATION_TIMEDOUT && running_) {
            LOG_WARN("Streamable HTTP event stream interrupted: " + std::string(curl_easy_strerror(res)));
        }
        if (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

void MCPClient::handleStreamableHttpEvent(const std::string& event_data) {
    MCPResponse response = parseJSONRPCResponse(event_data);
    if (response.id.empty()) {
        if (notification_callback_) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(event_data, root) && root["method"].asString() == "notifications/message") {
                const Json::Value& params = root["params"];
                notification_callback_(params["pluginName"].asString(), params["notification"].asString());
            }
        }
        return;
    }

    std::lock_guard<std::mutex> lock(queue_mutex_);
    response_queue_.push(response);
    queue_cv_.notify_one();
}

std::string MCPClient::getMcpEndpoint() const {
    std::string endpoint = config_.mcp_url;
    if (endpoint.size() >= 4 && endpoint.substr(endpoint.size() - 4) == "/mcp") {
        return endpoint;
    }
    if (!endpoint.empty() && endpoint.back() != '/') {
        endpoint += "/";
    }
    endpoint += "mcp";
    return endpoint;
}

size_t MCPClient::streamableHttpEventCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    MCPClient* client = static_cast<MCPClient*>(userdata);
    size_t total_size = size * nmemb;
    client->stream_response_buffer_.append(ptr, total_size);

    size_t pos = 0;
    while ((pos = client->stream_response_buffer_.find("\n\n")) != std::string::npos) {
        std::string event = client->stream_response_buffer_.substr(0, pos);
        client->stream_response_buffer_.erase(0, pos + 2);

        std::string event_data;
        std::istringstream iss(event);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.rfind("data:", 0) == 0) {
                std::string value = line.substr(5);
                size_t start = value.find_first_not_of(" ");
                if (start != std::string::npos) {
                    event_data += value.substr(start);
                }
            }
        }

        if (!event_data.empty()) {
            client->handleStreamableHttpEvent(event_data);
        }
    }

    return total_size;
}

size_t MCPClient::streamableHttpHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    MCPClient* client = static_cast<MCPClient*>(userdata);
    size_t total_size = size * nitems;
    std::string header(buffer, total_size);
    auto colon = header.find(':');
    if (colon == std::string::npos) {
        return total_size;
    }

    std::string name = header.substr(0, colon);
    std::string value = header.substr(colon + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());

    if (name == "mcp-session-id") {
        client->session_id_ = value;
    }
    return total_size;
}

} // namespace mcp
} // namespace agent_rpc
