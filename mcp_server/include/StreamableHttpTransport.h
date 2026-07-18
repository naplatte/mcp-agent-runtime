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

    // 这个类只能原地创建、原地使用，不能复制搬家，避免服务器资源被重复管理或管理失控
    StreamableHttp(const StreamableHttp&) = delete;
    StreamableHttp& operator=(const StreamableHttp&) = delete;
    StreamableHttp(StreamableHttp&&) = delete;
    StreamableHttp& operator=(StreamableHttp&&) = delete;

    // 读写/异步读写
    std::pair<size_t, std::string> Read() override;
    void Write(const std::string& json_data) override;
    std::future<std::pair<size_t, std::string>> ReadAsync() override;
    std::future<void> WriteAsync(const std::string& json_data) override;

    // 基本信息
    std::string GetName() override { return "streamable-http"; }
    std::string GetVersion() override { return "2025-11-25"; }
    int GetPort() override { return port_; }

    // 生命周期
    bool Start() override;
    void Stop() override;
    bool IsRunning() override { return server_running_.load(); }

private:
    // response元信息
    struct PendingResponse {
        std::mutex mutex;
        std::condition_variable cv; // 让POST/mcp的线程睡着等待，直到response被write()写回
        std::string body;
        bool ready{false};
    };

    void SetupRoutes(); // 路由注册
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

    // 请求队列
    std::queue<std::string> incoming_messages_;
    std::mutex incoming_mutex_;
    std::condition_variable incoming_cv_;

    // event stream 推送队列
    std::queue<std::string> outgoing_stream_messages_;
    std::mutex outgoing_mutex_;
    std::condition_variable outgoing_cv_;

    // 存放server对于post请求生成的response，是按 id 匹配的等待表
    std::map<std::string, std::shared_ptr<PendingResponse>> pending_responses_;
    std::mutex pending_mutex_;
};

} // namespace vx::transport

#endif // MCP_SERVER_STREAMABLE_HTTP_TRANSPORT_H
