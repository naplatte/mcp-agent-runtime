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
#include <cerrno>
#include <csignal>
#include <pthread.h>

namespace {
// A peer may exit between checking connected_ and writing a cancellation.
// Block SIGPIPE only in this calling thread, without changing the application's signal handlers.
ssize_t writePipe(int fd, const char* data, size_t length) {
    sigset_t blocked, previous, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) { errno = EIO; return -1; }
    sigpending(&pending);
    const bool already_pending = sigismember(&pending, SIGPIPE);
    const ssize_t result = write(fd, data, length);
    const int error = errno;
    if (result < 0 && error == EPIPE && !already_pending) {
        const timespec no_wait{0, 0};
        while (sigtimedwait(&blocked, nullptr, &no_wait) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = error;
    return result;
}

// Parse lines incrementally: chunks may split CRLF, data fields or event boundaries.
struct HttpResponseBuffer {
    std::string content_type;
    std::string body;
    std::string line_buffer;
    std::string event_data;
    std::function<void(const std::string&)> on_event;
    std::function<void(const std::string&)> on_session;

    void feed(const char* data, size_t length) {
        if (content_type.find("text/event-stream") == std::string::npos) {
            body.append(data, length);
            return;
        }
        line_buffer.append(data, length);
        size_t newline;
        while ((newline = line_buffer.find('\n')) != std::string::npos) {
            std::string line = line_buffer.substr(0, newline);
            line_buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                if (!event_data.empty()) {
                    event_data.pop_back();
                    on_event(event_data);
                    event_data.clear();
                }
            } else if (line.rfind("data:", 0) == 0) {
                std::string value = line.substr(5);
                if (!value.empty() && value.front() == ' ') value.erase(0, 1);
                event_data += value + "\n";
            }
            // SSE comments (including keepalive) do not complete or extend a request.
        }
    }
};

size_t receiveHttpBody(char* data, size_t size, size_t count, void* context) {
    try {
        static_cast<HttpResponseBuffer*>(context)->feed(data, size * count);
        return size * count;
    } catch (...) {
        return 0; // Never let a C++ exception cross libcurl's C callback boundary.
    }
}

size_t receiveHttpHeader(char* data, size_t size, size_t count, void* context) {
    try {
        auto& response = *static_cast<HttpResponseBuffer*>(context);
        const size_t length = size * count;
        std::string header(data, length);
        const auto colon = header.find(':');
        if (colon == std::string::npos) return length;
        std::string name = header.substr(0, colon);
        std::string value = header.substr(colon + 1);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::tolower(ch); });
        const auto begin = value.find_first_not_of(" \t\r\n");
        const auto end = value.find_last_not_of(" \t\r\n");
        value = begin == std::string::npos ? "" : value.substr(begin, end - begin + 1);
        if (name == "content-type") response.content_type = value;
        if (name == "mcp-session-id" && response.on_session) response.on_session(value);
        return length;
    } catch (...) {
        return 0;
    }
}
} // namespace

namespace agent_rpc {
namespace mcp {

// MCPClient 实现
MCPClient::MCPClient() {
    // Logger 使用全局宏，不需要实例化
    static std::atomic<unsigned long long> client_ids{0};
    request_id_prefix_ = std::to_string(getpid()) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
        std::to_string(client_ids.fetch_add(1)) + "_";
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
    
    if (config.request_timeout_ms <= 0 || config.connect_timeout_ms <= 0 || config.cancellation_timeout_ms <= 0) {
        LOG_ERROR("MCP timeouts must be positive");
        return false;
    }
    // EOF marks disconnected before the reader/cancellation threads are joined.
    if (running_ || notification_thread_.joinable() || stream_event_thread_.joinable() ||
        cancellation_thread_.joinable() || server_pid_ != -1 || curl_handle_) disconnect();
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
    
    if (success) cancellation_thread_ = std::thread(&MCPClient::processCancellations, this);
    // 6. 未识别的传输类型会保持 false
    return success;
}

MCPTransportType MCPClient::getTransportType() const {
    return transport_type_;
}

void MCPClient::disconnect() {
    if (!connected_ && !running_ && server_pid_ == -1 && !curl_handle_) {
        return;
    }
    
    running_ = false;
    connected_ = false;
    queue_cv_.notify_all();
    cancellation_cv_.notify_all();
    if (cancellation_thread_.joinable()) cancellation_thread_.join();
    
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
    request.id = request_id_prefix_ + "list_tools_" + std::to_string(next_request_id_.fetch_add(1));
    
    MCPResponse response = executeRequest(request);
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
    request.id = request_id_prefix_ + "call_tool_" + std::to_string(next_request_id_.fetch_add(1));
    
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
    
    response = executeRequest(request);
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
    request.id = request_id_prefix_ + "list_prompts_" + std::to_string(next_request_id_.fetch_add(1));
    
    MCPResponse response = executeRequest(request);
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
    request.id = request_id_prefix_ + "get_prompt_" + std::to_string(next_request_id_.fetch_add(1));
    
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
    
    request.params = params.toStyledString();
    response = executeRequest(request);
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
    request.id = request_id_prefix_ + "list_resources_" + std::to_string(next_request_id_.fetch_add(1));
    
    MCPResponse response = executeRequest(request);
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
    request.id = request_id_prefix_ + "read_resource_" + std::to_string(next_request_id_.fetch_add(1));
    
    // 构建参数
    Json::Value params;
    params["uri"] = uri;
    
    request.params = params.toStyledString();
    response = executeRequest(request);
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
    std::lock_guard<std::mutex> write_lock(stdio_write_mutex_);
    if (!running_ || stdin_pipe_ == -1) {
        LOG_ERROR("MCP server stdin pipe not available");
        return false;
    }
    
    std::string json_request = buildJSONRPCRequest(request);
    json_request += "\n";  // MCP协议使用换行符分隔消息
    
    size_t offset = 0;
    while (offset < json_request.size()) {
        ssize_t written = writePipe(stdin_pipe_, json_request.data() + offset, json_request.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            LOG_ERROR("Failed to write request to MCP server");
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    
    return true;
}

MCPResponse MCPClient::executeRequest(const MCPRequest& request) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        auto& pending = pending_requests_[request.id];
        pending.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.request_timeout_ms);
    }
    if (!sendRequest(request)) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_requests_.erase(request.id);
        return {request.id, "", "Failed to send request", true};
    }
    return receiveResponse(request);
}

MCPResponse MCPClient::receiveResponse(const MCPRequest& request) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    const auto deadline = pending_requests_.at(request.id).deadline;
    queue_cv_.wait_until(lock, deadline, [this, &request] {
        return pending_requests_.at(request.id).ready || !connected_;
    });
    auto& pending = pending_requests_.at(request.id);
    if (pending.ready) {
        MCPResponse response = std::move(pending.response);
        pending_requests_.erase(request.id);
        return response;
    }
    pending_requests_.erase(request.id);
    const bool disconnected = !connected_;
    lock.unlock();
    if (!disconnected) cancelRequest(request);
    return {request.id, "", disconnected ? "Connection closed" : "Request timeout", true};
}

void MCPClient::publishResponse(const MCPResponse& response) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    auto it = pending_requests_.find(response.id);
    if (it == pending_requests_.end() || it->second.ready) return;
    it->second.response = response;
    it->second.ready = true;
    queue_cv_.notify_all();
}

void MCPClient::cancelRequest(const MCPRequest& request) {
    if (request.id.empty() || request.method == "initialize" || !running_) return;
    Json::Value params;
    params["requestId"] = request.id;
    params["reason"] = "Request timeout";
    MCPRequest notification{"notifications/cancelled", params.toStyledString(), ""};
    {
        std::lock_guard<std::mutex> lock(cancellation_mutex_);
        cancellation_queue_.push(std::move(notification));
    }
    cancellation_cv_.notify_one();
}

void MCPClient::processCancellations() {
    while (running_) {
        MCPRequest notification;
        {
            std::unique_lock<std::mutex> lock(cancellation_mutex_);
            cancellation_cv_.wait(lock, [this] { return !running_ || !cancellation_queue_.empty(); });
            if (!running_) { cancellation_queue_ = {}; return; }
            notification = std::move(cancellation_queue_.front());
            cancellation_queue_.pop();
        }
        if (!sendRequest(notification)) LOG_WARN("Failed to send cancellation notification");
    }
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
                    handleStreamableHttpEvent(message);
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
    connected_ = false;
    queue_cv_.notify_all();
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
    
    std::lock_guard<std::mutex> write_lock(stdio_write_mutex_);
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
    if (!request.id.empty()) root["id"] = request.id;
    
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
        if (reader.parse(response, root) && root.isObject() && root.isMember("id") &&
            (root.isMember("result") || root.isMember("error"))) {
            mcp_response.id = root["id"].asString();
            
            if (root.isMember("error")) {
                mcp_response.is_error = true;
                mcp_response.error = root["error"]["message"].asString();
            } else {
                mcp_response.is_error = false;
                mcp_response.result = root["result"].toStyledString();
            }
        } else {
            mcp_response.is_error = true;
            mcp_response.error = "Invalid JSON-RPC response";
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
    // libcurl global state outlives individual clients, including concurrent clients.
    static std::once_flag initialized;
    static CURLcode init_result = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { init_result = curl_global_init(CURL_GLOBAL_ALL); });
    if (init_result != CURLE_OK) return false;
    curl_handle_ = curl_easy_init();
    return curl_handle_ != nullptr;
}

void MCPClient::disconnectStreamableHttp() {
    if (stream_event_thread_.joinable()) stream_event_thread_.join();
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_id_.clear();
}

bool MCPClient::sendRequestStreamableHttp(const MCPRequest& request) {
    if (!connected_) return false;
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    HttpResponseBuffer buffer;
    bool terminal_received = false;
    buffer.on_session = [this](const std::string& session) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_ = session;
    };
    buffer.on_event = [this, &request, &terminal_received](const std::string& message) {
        handleStreamableHttpEvent(message);
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(message, root) && root.isObject() && root["id"].asString() == request.id &&
            (root.isMember("result") || root.isMember("error"))) terminal_received = true;
    };
    struct ProgressState { MCPClient* client; bool* terminal; } progress{this, &terminal_received};
    const std::string endpoint = getMcpEndpoint();
    const std::string payload = buildJSONRPCRequest(request);
    const int timeout = request.id.empty() ? config_.cancellation_timeout_ms : config_.request_timeout_ms;
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config_.connect_timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receiveHttpHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveHttpBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            auto* state = static_cast<ProgressState*>(context);
            return !state->client->running_ || *state->terminal;
        });
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    headers = curl_slist_append(headers, "MCP-Protocol-Version: 2025-11-25");
    if (!request.id.empty()) {
        // Opt in to request-scoped SSE/disconnect cancellation without changing the protocol era.
        headers = curl_slist_append(headers, "X-MCP-Cancel-On-Disconnect: true");
    }
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!session_id_.empty()) {
            const std::string session = "MCP-Session-Id: " + session_id_;
            headers = curl_slist_append(headers, session.c_str());
        }
    }
    if (!config_.api_key.empty()) {
        const std::string auth = "Authorization: Bearer " + config_.api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (!config_.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    const CURLcode status = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl); // Also closes an unfinished request-scoped response stream.

    if (terminal_received) return true;
    if (status != CURLE_OK) {
        if (request.id.empty()) return false;
        const bool timed_out = status == CURLE_OPERATION_TIMEDOUT;
        if (timed_out) cancelRequest(request); // Best-effort legacy cancellation, sent off the caller's path.
        publishResponse({request.id, "", timed_out ? "Request timeout" :
            (!running_ ? "Connection closed" : std::string(curl_easy_strerror(status))), true});
        return true;
    }
    if (request.id.empty()) return http_code >= 200 && http_code < 300;
    if (!buffer.body.empty()) {
        auto response = parseJSONRPCResponse(buffer.body);
        if (response.id == request.id) {
            publishResponse(response);
            return true;
        }
    }
    publishResponse({request.id, "", http_code >= 400 ? "HTTP " + std::to_string(http_code) :
        "Missing or mismatched JSON-RPC response", true});
    return true;
}

void MCPClient::processStreamableHttpEvents() {
    while (running_) {
        CURL* curl = curl_easy_init();
        if (!curl) return;
        HttpResponseBuffer buffer; // Reconnects cannot splice partial frames from different streams.
        buffer.on_event = [this](const std::string& message) { handleStreamableHttpEvent(message); };
        buffer.on_session = [this](const std::string& session) {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_id_ = session;
        };
        const std::string endpoint = getMcpEndpoint();
        curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveHttpBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receiveHttpHeader);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 0L); // Long-lived events are independent of tool deadlines.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config_.connect_timeout_ms));
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
            +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return !static_cast<MCPClient*>(context)->running_;
            });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        headers = curl_slist_append(headers, "Cache-Control: no-cache");
        headers = curl_slist_append(headers, "MCP-Protocol-Version: 2025-11-25");
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (!session_id_.empty()) {
                const std::string session = "MCP-Session-Id: " + session_id_;
                headers = curl_slist_append(headers, session.c_str());
            }
        }
        if (!config_.api_key.empty()) {
            const std::string auth = "Authorization: Bearer " + config_.api_key;
            headers = curl_slist_append(headers, auth.c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        if (!config_.verify_ssl) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        const CURLcode status = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (status != CURLE_OK && running_) LOG_WARN("HTTP event stream interrupted");
        if (running_) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void MCPClient::handleStreamableHttpEvent(const std::string& message) {
    try {
        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(message, root) || !root.isObject()) return;
        if (!root.isMember("id")) {
            if (notification_callback_ && root["method"].asString() == "notifications/message") {
                const auto& params = root["params"];
                notification_callback_(params["pluginName"].asString(), params["notification"].asString());
            }
            return;
        }
        if (root.isMember("result") || root.isMember("error")) publishResponse(parseJSONRPCResponse(message));
    } catch (const std::exception& error) {
        LOG_WARN("Invalid MCP event or notification callback failed: " + std::string(error.what()));
    }
}

std::string MCPClient::getMcpEndpoint() const {
    std::string endpoint = config_.mcp_url;
    if (endpoint.size() >= 4 && endpoint.substr(endpoint.size() - 4) == "/mcp") return endpoint;
    if (!endpoint.empty() && endpoint.back() != '/') endpoint += "/";
    return endpoint + "mcp";
}

} // namespace mcp
} // namespace agent_rpc
