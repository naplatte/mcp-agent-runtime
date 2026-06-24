#include "StreamableHttpTransport.h"

#include "aixlog.hpp"
#include "json.hpp"

#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace vx::transport {

namespace {
constexpr auto kPostResponseTimeout = std::chrono::seconds(30);
constexpr auto kPingInterval = std::chrono::seconds(15);

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

StreamableHttp::StreamableHttp(const int port, std::string host)
    : host_(std::move(host)),
      port_(port),
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
            if (it != pending_responses_.end()) {
                pending = it->second;
            }
        }

        if (pending) {
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                pending->body = json_data;
                pending->ready = true;
            }
            pending->cv.notify_one();
            return;
        }
    }

    QueueStreamMessage(json_data);
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

    server_->Delete("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        HandleMcpDelete(req, res);
    });
}

void StreamableHttp::HandleMcpPost(const httplib::Request& req, httplib::Response& res) {
    SetCommonHeaders(res);

    if (!ValidateOrigin(req, res) || !ValidateSession(req, res)) {
        return;
    }

    if (req.body.empty()) {
        res.status = 400;
        res.set_content("{\"error\":\"Empty message\"}", "application/json");
        return;
    }

    const std::string id = ExtractMessageId(req.body);
    if (id.empty()) {
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
        pending_responses_[id] = pending;
    }

    {
        std::lock_guard<std::mutex> lock(incoming_mutex_);
        incoming_messages_.push(req.body);
    }
    incoming_cv_.notify_one();

    std::unique_lock<std::mutex> lock(pending->mutex);
    const bool ready = pending->cv.wait_for(lock, kPostResponseTimeout, [pending]() {
        return pending->ready;
    });

    {
        std::lock_guard<std::mutex> pending_lock(pending_mutex_);
        pending_responses_.erase(id);
    }

    if (!ready || pending->body.empty()) {
        res.status = 504;
        res.set_content("{\"error\":\"Timed out waiting for JSON-RPC response\"}", "application/json");
        return;
    }

    res.status = 200;
    res.set_content(pending->body, "application/json");
}

void StreamableHttp::HandleMcpStream(const httplib::Request& req, httplib::Response& res) {
    SetCommonHeaders(res);

    if (!ValidateOrigin(req, res) || !ValidateSession(req, res)) {
        return;
    }

    if (!HeaderContains(req, "Accept", "text/event-stream")) {
        res.status = 406;
        res.set_content("{\"error\":\"GET /mcp requires Accept: text/event-stream\"}", "application/json");
        return;
    }

    client_connected_.store(true);
    stream_active_.store(true);
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");

    auto last_ping = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());

    res.set_content_provider(
        "text/event-stream",
        [this, last_ping](size_t, httplib::DataSink& sink) -> bool {
            auto terminate = [this]() -> bool {
                stream_active_.store(false);
                client_connected_.store(false);
                outgoing_cv_.notify_all();
                incoming_cv_.notify_all();
                return false;
            };

            try {
                if (std::chrono::steady_clock::now() - *last_ping >= kPingInterval) {
                    const char* ping = ": ping\n\n";
                    if (!sink.write(ping, std::strlen(ping))) {
                        return terminate();
                    }
                    *last_ping = std::chrono::steady_clock::now();
                }

                std::unique_lock<std::mutex> lock(outgoing_mutex_);
                outgoing_cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() {
                    return !outgoing_stream_messages_.empty() || !stream_active_.load();
                });

                if (!stream_active_.load()) {
                    return terminate();
                }

                if (!outgoing_stream_messages_.empty()) {
                    std::string message = std::move(outgoing_stream_messages_.front());
                    outgoing_stream_messages_.pop();
                    lock.unlock();

                    std::string event_msg = "data: " + message + "\n\n";
                    if (!sink.write(event_msg.data(), event_msg.size())) {
                        return terminate();
                    }
                }

                return true;
            } catch (const std::exception& ex) {
                LOG(ERROR) << "Exception in Streamable HTTP content provider: " << ex.what() << std::endl;
                return terminate();
            }
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
        "Content-Type, Accept, Authorization, x-api-key, MCP-Session-Id, MCP-Protocol-Version");
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
        if (root["id"].is_string()) {
            return root["id"].get<std::string>();
        }
        return root["id"].dump();
    } catch (...) {
        return "";
    }
}

} // namespace vx::transport
