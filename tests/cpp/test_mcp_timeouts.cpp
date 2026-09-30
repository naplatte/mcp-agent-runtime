#include "agent_rpc/mcp/mcp_client.h"
#include "httplib.h"
#include "json.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace agent_rpc::mcp;
using namespace std::chrono_literals;
using json = nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

long elapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

std::string sleepArgs(int duration) {
    return json{{"milliseconds", duration}}.dump();
}

void requireSleep(const MCPResponse& response, int duration) {
    require(!response.is_error, "sleep failed: " + response.error);
    const auto result = json::parse(response.result);
    require(!result.value("isError", false), "tool execution failed");
    require(result["content"][0]["text"] == "Waited for " + std::to_string(duration) + " milliseconds",
        "response was delivered to the wrong request");
}

MCPConnectionConfig stdioConfig(const std::string& server, const std::string& plugins, int timeout) {
    MCPConnectionConfig config;
    config.server_path = server;
    config.server_args = {"-p", plugins};
    config.request_timeout_ms = timeout;
    return config;
}

MCPConnectionConfig httpConfig(int port, int timeout) {
    MCPConnectionConfig config;
    config.transport = MCPTransportType::STREAMABLE_HTTP;
    config.mcp_url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    config.request_timeout_ms = timeout;
    return config;
}

int freePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        throw std::runtime_error("bind failed");
    }
    socklen_t length = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
    const int port = ntohs(address.sin_port);
    close(fd);
    return port;
}

struct HttpServerProcess {
    pid_t pid = -1;
    int port = freePort();
    HttpServerProcess(const std::string& executable, const std::string& plugins, int timeout = 3000) {
        const auto portArg = std::to_string(port);
        const auto timeoutArg = std::to_string(timeout);
        pid = fork();
        require(pid >= 0, "fork failed");
        if (pid == 0) {
            int nullFd = open("/dev/null", O_RDWR);
            dup2(nullFd, STDIN_FILENO);
            dup2(nullFd, STDOUT_FILENO);
            close(nullFd);
            execl(executable.c_str(), executable.c_str(), "-p", plugins.c_str(),
                "--streamable-http", "--http-port", portArg.c_str(),
                "--request-timeout-ms", timeoutArg.c_str(), "--sse-keepalive-ms", "25",
                static_cast<char*>(nullptr));
            _exit(127);
        }
        httplib::Client health("127.0.0.1", port);
        health.set_connection_timeout(0, 100000);
        for (int i = 0; i < 250; ++i) {
            if (auto response = health.Get("/health"); response && response->status == 200) return;
            std::this_thread::sleep_for(20ms);
        }
        stop();
        throw std::runtime_error("HTTP server did not start");
    }
    void stop() {
        if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); pid = -1; }
    }
    ~HttpServerProcess() { stop(); }
};

// Deliberately ignores cancellation and sends late/out-of-order responses.
int mockStdio() {
    std::mutex output;
    std::vector<std::thread> jobs;
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto request = json::parse(line);
        if (!request.contains("id")) continue;
        jobs.emplace_back([request, &output] {
            const int duration = request["params"]["arguments"]["milliseconds"];
            if (duration == -1) _exit(0); // Simulate an unexpected server exit during a request.
            std::this_thread::sleep_for(std::chrono::milliseconds(duration));
            const json reply{{"jsonrpc", "2.0"}, {"id", request["id"]},
                {"result", {{"isError", false}, {"content", json::array({
                    {{"type", "text"}, {"text", "Waited for " + std::to_string(duration) + " milliseconds"}}
                })}}}};
            std::lock_guard<std::mutex> lock(output);
            std::cout << reply.dump() << std::endl;
        });
    }
    for (auto& job : jobs) job.join();
    return 0;
}

void testStdioCancellation(const std::string& server, const std::string& plugins) {
    MCPClient client;
    require(client.connect(stdioConfig(server, plugins, 200)), "stdio connect failed");
    require(!client.listTools().empty(), "tools not loaded");
    auto start = std::chrono::steady_clock::now();
    auto response = client.callTool("sleep", sleepArgs(2000));
    require(response.is_error && response.error == "Request timeout", "missing local timeout");
    require(elapsedMs(start) >= 150 && elapsedMs(start) < 700, "configured stdio deadline ignored");
    start = std::chrono::steady_clock::now();
    requireSleep(client.callTool("sleep", sleepArgs(10)), 10);
    require(elapsedMs(start) < 180, "cancelled tool kept the worker blocked");

    // A quick request can finish before the earlier slow request, without stealing its response.
    auto slow = std::async(std::launch::async, [&] { return client.callTool("sleep", sleepArgs(120)); });
    std::this_thread::sleep_for(20ms);
    require(!client.listTools().empty(), "out-of-order tools/list response lost");
    requireSleep(slow.get(), 120);
    client.disconnect();
}

void testLateResponseAndDisconnect(const std::string& self) {
    MCPClient client;
    auto config = stdioConfig(self, "unused", 100);
    config.server_args = {"--mock-stdio"};
    require(client.connect(config), "mock stdio connect failed");
    require(client.callTool("sleep", sleepArgs(220)).error == "Request timeout", "mock did not time out");
    requireSleep(client.callTool("sleep", sleepArgs(10)), 10);
    std::this_thread::sleep_for(140ms); // Let the original response arrive after its pending entry was removed.
    requireSleep(client.callTool("sleep", sleepArgs(20)), 20);
    require(client.callTool("sleep", sleepArgs(-1)).error == "Connection closed", "EOF did not wake caller");
    require(client.connect(config), "reconnect after EOF failed");
    requireSleep(client.callTool("sleep", sleepArgs(10)), 10);
    client.disconnect();

    config.request_timeout_ms = 5000;
    require(client.connect(config), "reconnect failed");
    auto waiting = std::async(std::launch::async, [&] { return client.callTool("sleep", sleepArgs(2000)); });
    std::this_thread::sleep_for(50ms);
    const auto start = std::chrono::steady_clock::now();
    client.disconnect();
    const auto response = waiting.get();
    require(response.error == "Connection closed", "disconnect did not wake pending request");
    require(elapsedMs(start) < 1000, "disconnect waited for request deadline");
}

void testHttpCancellation(const std::string& server, const std::string& plugins) {
    HttpServerProcess process(server, plugins);
    MCPClient client;
    require(client.connect(httpConfig(process.port, 200)), "HTTP connect failed");
    auto start = std::chrono::steady_clock::now();
    const auto response = client.callTool("sleep", sleepArgs(2000));
    require(response.error == "Request timeout", "HTTP comments incorrectly extended deadline");
    require(elapsedMs(start) < 700, "HTTP cancellation extended caller wait");
    requireSleep(client.callTool("sleep", sleepArgs(10)), 10);

    httplib::Client raw("127.0.0.1", process.port);
    raw.set_read_timeout(3);
    const json request{{"jsonrpc", "2.0"}, {"id", "sse-test"}, {"method", "tools/call"},
        {"params", {{"name", "sleep"}, {"arguments", {{"milliseconds", 180}}}}}};
    const httplib::Headers headers{{"Accept", "text/event-stream"}, {"X-MCP-Cancel-On-Disconnect", "true"}};
    const auto streamed = raw.Post("/mcp", headers, request.dump(), "application/json");
    require(streamed && streamed->status == 200, "request SSE failed");
    require(streamed->body.find(": keepalive\n\n") != std::string::npos, "no SSE keepalive emitted");
    require(streamed->body.find("Waited for 180") != std::string::npos, "SSE final response missing");

    // Abort the request stream without sending notifications/cancelled.
    auto abandoned = request;
    abandoned["id"] = "disconnect-test";
    abandoned["params"]["arguments"]["milliseconds"] = 2000;
    httplib::Request abortRequest;
    abortRequest.method = "POST";
    abortRequest.path = "/mcp";
    abortRequest.headers = headers;
    abortRequest.set_header("Content-Type", "application/json");
    abortRequest.body = abandoned.dump();
    abortRequest.content_receiver = [](const char*, size_t, uint64_t, uint64_t) { return false; };
    const auto closed = raw.send(abortRequest);
    require(!closed, "test did not close the stream early");
    requireSleep(client.callTool("sleep", sleepArgs(10)), 10);
    client.disconnect();
}

void testServerDeadline(const std::string& server, const std::string& plugins) {
    HttpServerProcess process(server, plugins, 120);
    httplib::Client raw("127.0.0.1", process.port);
    const json request{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
        {"params", {{"name", "sleep"}, {"arguments", {{"milliseconds", 2000}}}}}};
    const auto response = raw.Post("/mcp", request.dump(), "application/json");
    require(response && response->status == 504, "server deadline not enforced");
    const auto error = json::parse(response->body);
    require(error["id"] == 7 && error.contains("error") && !error.contains("result"), "invalid timeout error envelope");
    MCPClient client;
    require(client.connect(httpConfig(process.port, 500)), "HTTP connect failed");
    requireSleep(client.callTool("sleep", sleepArgs(10)), 10);
    client.disconnect();
}

void testFragmentedSseAndLongLivedGet() {
    httplib::Server server;
    std::atomic<int> getCount{0}, notificationCount{0};
    server.Post("/mcp", [](const httplib::Request& request, httplib::Response& response) {
        const auto input = json::parse(request.body);
        const json reply{{"jsonrpc", "2.0"}, {"id", input["id"]},
            {"result", {{"content", json::array({{{"type", "text"}, {"text", "Waited for 10 milliseconds"}}})}, {"isError", false}}}};
        std::string frame = ": keepalive\r\n\r\nevent: message\r\ndata: ";
        for (char ch : reply.dump(2)) frame += ch == '\n' ? "\r\ndata: " : std::string(1, ch);
        frame += "\r\n\r\n";
        response.set_chunked_content_provider("text/event-stream",
            [frame](size_t, httplib::DataSink& sink) {
                for (size_t i = 0; i < frame.size(); i += 3) {
                    if (!sink.write(frame.data() + i, std::min<size_t>(3, frame.size() - i))) return false;
                }
                sink.done();
                return true;
            });
    });
    server.Get("/mcp", [&](const httplib::Request&, httplib::Response& response) {
        ++getCount;
        response.set_chunked_content_provider("text/event-stream",
            [](size_t offset, httplib::DataSink& sink) {
                std::this_thread::sleep_for(25ms);
                if (offset == 0) {
                    const std::string frame = "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\",\"params\":{\"pluginName\":\"test\",\"notification\":\"hello\"}}\n\n";
                    return sink.write(frame.data(), frame.size());
                }
                return sink.write(": keepalive\n\n", 13);
            });
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    require(port > 0, "mock HTTP bind failed");
    std::thread listener([&] { server.listen_after_bind(); });
    struct StopServer {
        httplib::Server& server;
        std::thread& thread;
        ~StopServer() { server.stop(); if (thread.joinable()) thread.join(); }
    } cleanup{server, listener};
    MCPClient client;
    client.setNotificationCallback([&](const std::string&, const std::string&) { ++notificationCount; });
    require(client.connect(httpConfig(port, 500)), "mock HTTP connect failed");
    requireSleep(client.callTool("sleep", sleepArgs(10)), 10);
    std::this_thread::sleep_for(2300ms);
    require(getCount == 1 && notificationCount == 1, "GET stream reconnected due to the old two-second timeout");
    const auto start = std::chrono::steady_clock::now();
    client.disconnect();
    require(elapsedMs(start) < 1500, "long-lived GET could not be stopped");
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--mock-stdio") return mockStdio();
    if (argc != 3) return 2;
    try {
        testStdioCancellation(argv[1], argv[2]);
        std::cout << "PASS stdio deadline, cooperative cancellation and out-of-order IDs\n";
        testLateResponseAndDisconnect(argv[0]);
        std::cout << "PASS late responses, reconnect and disconnect cleanup\n";
        testHttpCancellation(argv[1], argv[2]);
        std::cout << "PASS HTTP deadline, cancellation and SSE keepalive\n";
        testServerDeadline(argv[1], argv[2]);
        std::cout << "PASS server deadline and worker recovery\n";
        testFragmentedSseAndLongLivedGet();
        std::cout << "PASS fragmented SSE parser and long-lived GET shutdown\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
