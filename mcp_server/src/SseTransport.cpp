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
//   included in all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
//  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
//  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// -----------------------------------------------------------------------------
//
//  Contributors:
//    - Erdenebileg Byamba (https://github.com/erd3n)
//          * Contribution: Initial implementation of SSE Transport
//    - Giuseppe Mastrangelo (https://github.com/peppemas)
//          * Contribution: Fixed code to be compatible with MCP Server specification
//
// -----------------------------------------------------------------------------

#include "SseTransport.h"

#include <iostream>
#include "httplib.h"
#include <chrono>
#include <utility>
#include <iomanip>

#include "aixlog.hpp"
#include "json.hpp"

namespace vx::transport {

    SSE::SSE(const int port, std::string host) : host_(std::move(host)), port_(port), server_(std::make_unique<httplib::Server>()) {
        SetupRoutes();
    }

    SSE::~SSE() {
        SSE::Stop();
    }

    std::pair<size_t, std::string> SSE::Read() {
        std::unique_lock<std::mutex> lock(incoming_mutex_);

        LOG(TRACE) << "WAITING FOR LOCK TO BE RELEASED" << std::endl;
        LOG(TRACE) << "incoming_messages_: " << incoming_messages_.empty() << std::endl;
        LOG(TRACE) << "server_running: " << server_running_.load() << std::endl;

        incoming_cv_.wait(lock, [this]() {
            return !incoming_messages_.empty() || !server_running_.load();
        });

        LOG(TRACE) << "LOCK RELEASED" << std::endl;

        if (!server_running_.load() && incoming_messages_.empty()) {
            return {0, ""};
        }

        if (!incoming_messages_.empty()) {
            std::string message = incoming_messages_.front();
            incoming_messages_.pop();
            return { message.length(), message };
        } else {
            return {0, ""};
        }
    }

    void SSE::Write(const std::string& json_data) {
        LOG(TRACE) << "RequestHandler delegated = " << json_data << std::endl;
        LOG(TRACE) << "is_client_connected = " << client_connected_.load() << std::endl;
        if (!client_connected_.load()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(outgoing_mutex_);
            outgoing_messages_.push(json_data);
        }

        outgoing_cv_.notify_one();
    }

    std::future<std::pair<size_t, std::string>> SSE::ReadAsync() {
        return std::async(std::launch::async, [this]() -> std::pair<size_t, std::string> {
            LOG(TRACE) << "READ ASYNC CALLED!!!" << std::endl;
            return Read();
        });
    }

    std::future<void> SSE::WriteAsync(const std::string& json_data) {
        return std::async(std::launch::async, [this, json_data] () {
            Write(json_data);
        });
    }

    bool SSE::Start() {
        if (server_running_.load()) {
            return false;
        }

        server_running_.store(true);

        server_thread_ = std::thread([this]() {
            LOG(INFO) << "Starting SSE server on " << host_ << ":" << port_ << std::endl;

            // 头文件说明:explicit SSE(int port = 8080, std::string  host = "127.0.0.1");
            // 默认监听 127.0.0.1:8080
            if (!server_->listen(host_.c_str(), port_)) {
                LOG(ERROR) << "Failed to start SSE server on " << host_ << ":" << port_ << std::endl;
                server_running_.store(false);
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return server_running_.load();
    }

    void SSE::Stop() {
        if (!server_running_.load()) {
            return;
        }

        server_running_.store(false);
        client_connected_.store(false);
        sse_active_.store(false);

        if (server_) {
            server_->stop();
        }

        if (server_thread_.joinable()) {
            server_thread_.join();
        }

        incoming_cv_.notify_all();
        outgoing_cv_.notify_all();
    }

    void SSE::SetupRoutes() {
        server_->Options("/.*", [this](const httplib::Request& req, httplib::Response& res) {
            HandleOptionsRequest(req, res);
        });

        // 收到get，执行lambda，返回一个json
        server_->Get("/health", [](const httplib::Request& req, httplib::Response& res) {
            res.set_content("{\"status\" : \"ok\"}", "application/json");
        });

        // 客户端发送 POST /messages 时，把请求转交给 HandlePostMessage()
        server_->Post("/messages", [this](const httplib::Request& req, httplib::Response& res) {
            HandlePostMessage(req, res);
        });

        // 客户端访问 GET /sse 时，进入 HandleSSEConnection()，建立持续推送消息的 SSE 长连接
        server_->Get("/sse", [this](const httplib::Request& req, httplib::Response& res) {
            HandleSSEConnection(req, res);
        });
    }

    void SSE::HandleSSEConnection(const httplib::Request& req, httplib::Response& res) {
        // 1. 记录客户端连接信息
        LOG(DEBUG) << "SSE client connected" << std::endl;
        LOG(DEBUG) << "Request headers:" << std::endl;
        for (const auto &header: req.headers) {
            LOG(DEBUG) << " - " << header.first << ": " << header.second << std::endl;
        }
        LOG(DEBUG) << "Request method: " << req.method << std::endl;
        LOG(DEBUG) << "Request path: " << req.path << std::endl;
        LOG(DEBUG) << "Request version: " << req.version << std::endl;
        LOG(DEBUG) << "Request remote address: " << req.remote_addr << std::endl;
        LOG(DEBUG) << "Request remote port: " << req.remote_port << std::endl;

        // 2. 设置 SSE 响应头并标记连接为活跃
        SetCORSHeaders(res);
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");

        client_connected_.store(true);
        sse_active_.store(true);

        // 3. 注册流式响应处理器，持续向客户端推送数据
        res.set_content_provider(
            "text/event-stream",
            [this](size_t offset, httplib::DataSink& sink) -> bool {
                using clock = std::chrono::steady_clock;
                static thread_local bool first_call = true;
                static thread_local auto last_ping = clock::now();
                const auto ping_interval = std::chrono::seconds(15);

                // 连接结束时清理状态，并唤醒所有等待线程
                auto terminate = [this]() -> bool {
                    sse_active_.store(false);
                    client_connected_.store(false);
                    outgoing_cv_.notify_all();
                    incoming_cv_.notify_all();
                    return false;
                };

                try {
                    // 4. 首次调用：生成会话 ID，并告知客户端消息提交地址
                    if (first_call) {
                        first_call = false;

                        std::string sessionId = vx::utils::SessionBuilder::GenerateUniqueSessionID();
                        std::string event_endpoint = "event: endpoint\ndata: /messages?session_id=" + sessionId + "\n\n";
                        if (!sink.write(event_endpoint.data(), event_endpoint.size())) {
                            LOG(ERROR) << "Failed to write event_endpoint message" << std::endl;
                            return terminate();
                        }
                        last_ping = clock::now();
                    }

                    // 5. 每 15 秒发送一次保活消息
                    if (clock::now() - last_ping >= ping_interval) {
                        const char* ping = ": ping\n\n";
                        if (!sink.write(ping, std::strlen(ping))) {
                            LOG(ERROR) << "Keep-alive write failed; client likely disconnected" << std::endl;
                            return terminate();
                        }
                        last_ping = clock::now();
                    }

                    // 6. 等待发送队列出现消息，最多等待 200ms
                    std::unique_lock<std::mutex> lock(outgoing_mutex_);
                    outgoing_cv_.wait_for(lock, std::chrono::milliseconds(200), [this]() {
                        return !outgoing_messages_.empty() || !sse_active_.load();
                    });

                    if (!sse_active_.load()) {
                        LOG(ERROR) << "SSE connection terminating (inactive)" << std::endl;
                        return terminate();
                    }

                    // 7. 检查客户端连接是否仍可写
#ifdef CPPHTTPLIB_HAS_SINK_IS_WRITABLE
                    if (!sink.is_writable()) {
                        LOG(DEBUG) << "Sink no longer writable; client likely disconnected" << std::endl;
                        return terminate();
                    }
#endif

                    // 8. 取出队列消息，包装成 SSE 格式后发送
                    if (!outgoing_messages_.empty()) {
                        std::string message = outgoing_messages_.front();
                        outgoing_messages_.pop();
                        lock.unlock();

                        std::string sse_msg = "data: " + message + "\n\n";
                        LOG(DEBUG) << "Sending SSE message: " << message << std::endl;

                        if (!sink.write(sse_msg.data(), sse_msg.size())) {
                            LOG(ERROR) << "Failed to write SSE message; client disconnected" << std::endl;
                            return terminate();
                        }
                    }

                    return true; // 保持连接，继续下一轮处理
                } catch (const std::exception& ex) {
                    LOG(ERROR) << "Exception in SSE content provider: " << ex.what() << std::endl;
                    return terminate();
                } catch (...) {
                    LOG(ERROR) << "Unknown exception in SSE content provider" << std::endl;
                    return terminate();
                }
            }
        );
    }

    void SSE::HandlePostMessage(const httplib::Request& req, httplib::Response& res) {
        SetCORSHeaders(res);

        // 检查客户端是否已连接
        if (!client_connected_.load()) {
            res.status = 503;
            res.set_content("{\"error\":\"No SSE connection\"}", "application/json");
            return;
        }

        // 获取请求消息体
        std::string message = req.body;
        if (message.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"Empty message\"}", "application/json");
            return;
        }

        // 将消息放入income队列（互斥访问）
        LOG(DEBUG) << "Received message via POST: " << message << std::endl;
        {
            std::lock_guard<std::mutex> lock(incoming_mutex_);
            incoming_messages_.push(message);
        }

        // 唤醒读线程(read())取消息
        incoming_cv_.notify_one();

        res.status = 200;
        res.set_content("{\"status\":\"received\"}", "application/json");
    }

    void SSE::HandleOptionsRequest(const httplib::Request& req, httplib::Response& res) {
        SetCORSHeaders(res);
        res.status = 200;
    }

    void SSE::SetCORSHeaders(httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, x-api-key");
        res.set_header("Access-Control-Expose-Headers", "Content-Type, Authorization, x-api-key");
        res.set_header("Access-Control-Max-Age", "86400");
    }

}
