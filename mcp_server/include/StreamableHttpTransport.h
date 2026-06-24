#ifndef MCP_SERVER_STREAMABLE_HTTP_TRANSPORT_H
#define MCP_SERVER_STREAMABLE_HTTP_TRANSPORT_H

#include "ITransport.h"
#include "httplib.h"
#include "SessionBuilder.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace vx::transport {

class StreamableHttp : public vx::ITransport {
public:
    explicit StreamableHttp(int port = 8080, std::string host = "127.0.0.1");
    ~StreamableHttp();

    StreamableHttp(const StreamableHttp&) = delete;
    StreamableHttp& operator=(const StreamableHttp&) = delete;
    StreamableHttp(StreamableHttp&&) = delete;
    StreamableHttp& operator=(StreamableHttp&&) = delete;

    std::pair<size_t, std::string> Read() override;
    void Write(const std::string& json_data) override;

    std::future<std::pair<size_t, std::string>> ReadAsync() override;
    std::future<void> WriteAsync(const std::string& json_data) override;

    std::string GetName() override { return "streamable-http"; }
    std::string GetVersion() override { return "2025-11-25"; }
    int GetPort() override { return port_; }

    bool Start() override;
    void Stop() override;
    bool IsRunning() override { return server_running_.load(); }

private:
    struct PendingResponse {
        std::mutex mutex;
        std::condition_variable cv;
        std::string body;
        bool ready{false};
    };

    void SetupRoutes();
    void HandleMcpPost(const httplib::Request& req, httplib::Response& res);
    void HandleMcpStream(const httplib::Request& req, httplib::Response& res);
    void HandleMcpDelete(const httplib::Request& req, httplib::Response& res);

    void QueueStreamMessage(const std::string& json_data);
    bool ValidateOrigin(const httplib::Request& req, httplib::Response& res) const;
    bool ValidateSession(const httplib::Request& req, httplib::Response& res) const;
    std::string GetSessionHeader(const httplib::Request& req) const;
    void SetCommonHeaders(httplib::Response& res) const;
    void HandleOptionsRequest(const httplib::Request& req, httplib::Response& res) const;
    static void SetCORSHeaders(httplib::Response& res);
    static std::string ExtractMessageId(const std::string& json_data);

    std::string host_;
    int port_;
    std::string session_id_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    std::atomic<bool> server_running_{false};
    std::atomic<bool> client_connected_{false};
    std::atomic<bool> stream_active_{false};

    std::queue<std::string> incoming_messages_;
    std::mutex incoming_mutex_;
    std::condition_variable incoming_cv_;

    std::queue<std::string> outgoing_stream_messages_;
    std::mutex outgoing_mutex_;
    std::condition_variable outgoing_cv_;

    std::map<std::string, std::shared_ptr<PendingResponse>> pending_responses_;
    std::mutex pending_mutex_;
};

} // namespace vx::transport

#endif // MCP_SERVER_STREAMABLE_HTTP_TRANSPORT_H
