#include "StreamableHttpTransport.h"

#include "aixlog.hpp"
#include "json.hpp"

#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace vx::transport {

namespace {
constexpr auto kPollInterval = std::chrono::milliseconds(50);

bool HeaderContains(const httplib::Request& req, const std::string& name, const std::string& value) {
    if (!req.has_header(name)) {
        return false;
    }
    return req.get_header_value(name).find(value) != std::string::npos;
}

bool OriginMatchesHost(const std::string& origin, const std::string& host) {
    if (host.empty()) {
        return false;
    }
    for (const auto& scheme : {"http://", "https://"}) {
        const std::string prefix = std::string(scheme) + host;
        if (origin == prefix || origin.rfind(prefix + ":", 0) == 0) {
            return true;
        }
    }
    return false;
}
} // namespace

StreamableHttp::StreamableHttp(const int port, std::string host, int response_timeout_ms, int keep_alive_interval_ms)
    : host_(std::move(host)),
      port_(port),
      response_timeout_ms_(response_timeout_ms),
      keep_alive_interval_ms_(keep_alive_interval_ms),
      session_id_(vx::utils::SessionBuilder::GenerateUniqueSessionID()),
      server_(std::make_unique<httplib::Server>()) {
    SetupRoutes();
}

StreamableHttp::~StreamableHttp() {
    StreamableHttp::Stop();
}

std::pair<size_t, std::string> StreamableHttp::Read() {
    std::unique_lock<std::mutex> lock(incoming_mutex_);
    incoming_cv_.wait(lock, [this]() {
        return !incoming_messages_.empty() || !server_running_.load();
    });

    if (!server_running_.load() && incoming_messages_.empty()) {
        return {0, ""};
    }

    std::string message = std::move(incoming_messages_.front());
    incoming_messages_.pop();
    return {message.length(), message};
}

void StreamableHttp::Write(const std::string& json_data) {
    const std::string id = ExtractMessageId(json_data);
    if (!id.empty()) {
        std::shared_ptr<PendingResponse> pending;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_responses_.find(id);
            if (it != pending_responses_.end()) pending = it->second;
        }
        if (pending) {
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (!pending->cancelled && !pending->ready) {
                pending->body = json_data;
                pending->ready = true;
                pending->cv.notify_one();
            }
        }
        return; // A late response must never leak into the notification stream.
    }
    QueueStreamMessage(json_data);
}

void StreamableHttp::CancelRequest(const std::string& request_id) {
    std::shared_ptr<PendingResponse> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_responses_.find(request_id);
        if (it != pending_responses_.end()) pending = it->second;
    }
    if (pending) {
        std::lock_guard<std::mutex> lock(pending->mutex);
        pending->cancelled = true;
        pending->cv.notify_one();
    }
}

void StreamableHttp::RemovePendingResponse(const std::string& id, const std::shared_ptr<PendingResponse>& pending) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_responses_.find(id);
    if (it != pending_responses_.end() && it->second == pending) pending_responses_.erase(it);
}

void StreamableHttp::QueueCancellation(const std::string& request_id) {
    auto notification = nlohmann::json{
        {"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
        {"params", {{"requestId", nlohmann::json::parse(request_id)}, {"reason", "HTTP request abandoned"}}}
    };
    {
        std::lock_guard<std::mutex> lock(incoming_mutex_);
        incoming_messages_.push(notification.dump());
    }
    incoming_cv_.notify_one();
}

std::future<std::pair<size_t, std::string>> StreamableHttp::ReadAsync() {
    return std::async(std::launch::async, [this]() {
        return Read();
    });
}

std::future<void> StreamableHttp::WriteAsync(const std::string& json_data) {
    return std::async(std::launch::async, [this, json_data]() {
        Write(json_data);
    });
}

bool StreamableHttp::Start() {
    if (server_running_.load()) {
        return false;
    }

    server_running_.store(true);
    server_thread_ = std::thread([this]() {
        LOG(INFO) << "Starting Streamable HTTP server on " << host_ << ":" << port_ << std::endl;
        if (!server_->listen(host_.c_str(), port_)) {
            LOG(ERROR) << "Failed to start Streamable HTTP server on " << host_ << ":" << port_ << std::endl;
            server_running_.store(false);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return server_running_.load();
}

void StreamableHttp::Stop() {
    const bool was_running = server_running_.exchange(false);
    if (!was_running && !server_thread_.joinable()) {
        return;
    }

    client_connected_.store(false);
    stream_active_.store(false);

    if (server_) {
        server_->stop();
    }

    incoming_cv_.notify_all();
    outgoing_cv_.notify_all();

    std::vector<std::shared_ptr<PendingResponse>> pending_responses;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto& item : pending_responses_) {
            pending_responses.push_back(item.second);
        }
        pending_responses_.clear();
    }

    for (auto& pending : pending_responses) {
        std::lock_guard<std::mutex> pending_lock(pending->mutex);
        pending->ready = true;
        pending->body.clear();
        pending->cv.notify_one();
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void StreamableHttp::SetupRoutes() {
    // CORS 预检
    server_->Options("/.*", [this](const httplib::Request& req, httplib::Response& res) {
        HandleOptionsRequest(req, res);
    });

    server_->Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
        SetCommonHeaders(res);
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    server_->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        HandleMcpPost(req, res);
    });

    server_->Get("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        HandleMcpStream(req, res);
    });

    // 关闭 event stream
    server_->Delete("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        HandleMcpDelete(req, res);
    });
}

void StreamableHttp::HandleMcpPost(const httplib::Request& req, httplib::Response& res) {
    SetCommonHeaders(res);
    if (!ValidateOrigin(req, res) || !ValidateSession(req, res)) return;
    auto request = nlohmann::json::parse(req.body, nullptr, false);
    if (!request.is_object() || !request.contains("method") || !request["method"].is_string()) {
        res.status = 400;
        res.set_content("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Invalid request\"}}", "application/json");
        return;
    }
    const std::string id = ExtractMessageId(req.body);
    if (id.empty()) {
        if (request.contains("id")) {
            res.status = 400;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(incoming_mutex_);
            incoming_messages_.push(req.body);
        }
        incoming_cv_.notify_one();
        res.status = 202;
        return;
    }
    auto pending = std::make_shared<PendingResponse>();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (!pending_responses_.emplace(id, pending).second) {
            res.status = 400;
            res.set_content("{\"error\":\"Duplicate request ID\"}", "application/json");
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(incoming_mutex_);
        incoming_messages_.push(req.body);
    }
    incoming_cv_.notify_one();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(response_timeout_ms_);
    const bool cancel_on_disconnect = req.get_header_value("X-MCP-Cancel-On-Disconnect") == "true";
    auto connection_closed = req.is_connection_closed;
    auto timeout_body = [id]() {
        return nlohmann::json{{"jsonrpc", "2.0"}, {"id", nlohmann::json::parse(id)},
            {"error", {{"code", -32000}, {"message", "Server request timeout"}}}}.dump();
    };
    if (cancel_on_disconnect && HeaderContains(req, "Accept", "text/event-stream")) {
        res.set_header("Cache-Control", "no-cache, no-transform");
        res.set_header("X-Accel-Buffering", "no");
        auto last_keepalive = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto finished = std::make_shared<std::atomic<bool>>(false);
        auto finish = [this, id, pending, finished](bool cancel) {
            if (finished->exchange(true)) return;
            RemovePendingResponse(id, pending);
            if (cancel && server_running_) QueueCancellation(id);
        };
        res.set_chunked_content_provider("text/event-stream",
            [this, pending, deadline, last_keepalive, finish, connection_closed, timeout_body]
            (size_t, httplib::DataSink& sink) -> bool {
                std::unique_lock<std::mutex> lock(pending->mutex);
                pending->cv.wait_for(lock, kPollInterval, [this, pending] {
                    return pending->ready || pending->cancelled || !server_running_;
                });
                if (pending->cancelled || !server_running_ || connection_closed()) {
                    lock.unlock();
                    finish(true);
                    return false;
                }
                if (pending->ready) {
                    const std::string event = "data: " + pending->body + "\n\n";
                    lock.unlock();
                    const bool written = sink.write(event.data(), event.size());
                    finish(false);
                    if (written) sink.done();
                    return written;
                }
                lock.unlock();
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    const std::string event = "data: " + timeout_body() + "\n\n";
                    const bool written = sink.write(event.data(), event.size());
                    finish(true);
                    if (written) sink.done();
                    return written;
                }
                if (keep_alive_interval_ms_ > 0 && now - *last_keepalive >= std::chrono::milliseconds(keep_alive_interval_ms_)) {
                    const std::string comment = ": keepalive\n\n";
                    if (!sink.write(comment.data(), comment.size())) { finish(true); return false; }
                    *last_keepalive = now;
                }
                return true;
            },
            [pending, finish](bool) {
                bool unfinished;
                {
                    std::lock_guard<std::mutex> lock(pending->mutex);
                    unfinished = !pending->ready;
                }
                finish(unfinished);
            });
        return;
    }

    // Keep legacy JSON responses; stream disconnect cancellation is explicit opt-in.
    std::unique_lock<std::mutex> lock(pending->mutex);
    while (!pending->ready && !pending->cancelled && server_running_ &&
           std::chrono::steady_clock::now() < deadline && !connection_closed()) {
        pending->cv.wait_for(lock, kPollInterval);
    }
    const bool ready = pending->ready;
    const bool cancelled = pending->cancelled;
    const std::string body = pending->body;
    lock.unlock();
    RemovePendingResponse(id, pending);
    if (!ready && !cancelled && (cancel_on_disconnect || std::chrono::steady_clock::now() >= deadline)) QueueCancellation(id);
    if (cancelled || !server_running_ || connection_closed()) { res.status = 204; return; }
    if (!ready) { res.status = 504; res.set_content(timeout_body(), "application/json"); return; }
    res.set_content(body, "application/json");
}

void StreamableHttp::HandleMcpStream(const httplib::Request& req, httplib::Response& res) {
    SetCommonHeaders(res);
    if (!ValidateOrigin(req, res) || !ValidateSession(req, res)) return;
    if (!HeaderContains(req, "Accept", "text/event-stream")) {
        res.status = 406;
        res.set_content("{\"error\":\"GET /mcp requires Accept: text/event-stream\"}", "application/json");
        return;
    }
    client_connected_ = true;
    stream_active_ = true;
    res.set_header("Cache-Control", "no-cache, no-transform");
    res.set_header("X-Accel-Buffering", "no");
    auto last_keepalive = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    auto connection_closed = req.is_connection_closed;
    res.set_chunked_content_provider("text/event-stream",
        [this, last_keepalive, connection_closed](size_t, httplib::DataSink& sink) -> bool {
            if (!stream_active_ || !server_running_ || connection_closed()) return false;
            const auto now = std::chrono::steady_clock::now();
            if (keep_alive_interval_ms_ > 0 && now - *last_keepalive >= std::chrono::milliseconds(keep_alive_interval_ms_)) {
                const std::string comment = ": keepalive\n\n";
                if (!sink.write(comment.data(), comment.size())) return false;
                *last_keepalive = now;
            }
            std::unique_lock<std::mutex> lock(outgoing_mutex_);
            outgoing_cv_.wait_for(lock, kPollInterval, [this] { return !outgoing_stream_messages_.empty() || !stream_active_; });
            if (!stream_active_) return false;
            if (!outgoing_stream_messages_.empty()) {
                const std::string event = "data: " + outgoing_stream_messages_.front() + "\n\n";
                outgoing_stream_messages_.pop();
                lock.unlock();
                return sink.write(event.data(), event.size());
            }
            return true;
        },
        [this](bool) {
            stream_active_ = false;
            client_connected_ = false;
            outgoing_cv_.notify_all();
        });
}

void StreamableHttp::HandleMcpDelete(const httplib::Request& req, httplib::Response& res) {
    SetCommonHeaders(res);

    if (!ValidateOrigin(req, res) || !ValidateSession(req, res)) {
        return;
    }

    stream_active_.store(false);
    client_connected_.store(false);
    outgoing_cv_.notify_all();
    res.status = 204;
}

void StreamableHttp::QueueStreamMessage(const std::string& json_data) {
    if (!client_connected_.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(outgoing_mutex_);
        outgoing_stream_messages_.push(json_data);
    }
    outgoing_cv_.notify_one();
}

bool StreamableHttp::ValidateOrigin(const httplib::Request& req, httplib::Response& res) const {
    if (!req.has_header("Origin")) {
        return true;
    }

    const std::string origin = req.get_header_value("Origin");
    if (OriginMatchesHost(origin, "localhost") ||
        OriginMatchesHost(origin, "127.0.0.1") ||
        OriginMatchesHost(origin, "[::1]") ||
        OriginMatchesHost(origin, host_)) {
        return true;
    }

    res.status = 403;
    res.set_content("{\"error\":\"Forbidden origin\"}", "application/json");
    return false;
}

bool StreamableHttp::ValidateSession(const httplib::Request& req, httplib::Response& res) const {
    const std::string session = GetSessionHeader(req);
    if (!session.empty() && session != session_id_) {
        res.status = 404;
        res.set_content("{\"error\":\"Unknown MCP session\"}", "application/json");
        return false;
    }
    return true;
}

std::string StreamableHttp::GetSessionHeader(const httplib::Request& req) const {
    if (req.has_header("MCP-Session-Id")) {
        return req.get_header_value("MCP-Session-Id");
    }
    return "";
}

void StreamableHttp::SetCommonHeaders(httplib::Response& res) const {
    SetCORSHeaders(res);
    res.set_header("MCP-Session-Id", session_id_);
    res.set_header("MCP-Protocol-Version", "2025-11-25");
}

void StreamableHttp::HandleOptionsRequest(const httplib::Request& req, httplib::Response& res) const {
    SetCORSHeaders(res);
    if (!ValidateOrigin(req, res)) {
        return;
    }
    res.status = 200;
}

void StreamableHttp::SetCORSHeaders(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    res.set_header(
        "Access-Control-Allow-Headers",
        "Content-Type, Accept, Authorization, x-api-key, MCP-Session-Id, MCP-Protocol-Version, X-MCP-Cancel-On-Disconnect");
    res.set_header(
        "Access-Control-Expose-Headers",
        "Content-Type, MCP-Session-Id, MCP-Protocol-Version");
    res.set_header("Access-Control-Max-Age", "86400");
}

std::string StreamableHttp::ExtractMessageId(const std::string& json_data) {
    try {
        auto root = nlohmann::json::parse(json_data);
        if (!root.contains("id") || root["id"].is_null()) {
            return "";
        }
        if (!root["id"].is_string() && !root["id"].is_number_integer()) return "";
        return root["id"].dump();
    } catch (...) {
        return "";
    }
}

} // namespace vx::transport
