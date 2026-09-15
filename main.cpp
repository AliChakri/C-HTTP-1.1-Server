#include "network/TcpServer.h"
#include "concurrency/TaskQueue.h"
#include "concurrency/ThreadPool.h"
#include "Utils/Logger.h"

#include <chrono>
#include <iostream>
#include <csignal>


// Static refrence to server instance for singal handling
static TcpServer* g_server = nullptr;

void signal_handler(int sig) {
    if (g_server) {
        g_server->stop(); // Non-blocking: signals EventLoop to enter STOPPING state
    }
}

int main() {
    // Register signal handlers BEFORE initializing server resources
    std::signal(SIGINT, signal_handler); //     Intercepts Ctrl+C
    std::signal(SIGTERM, signal_handler); //    Intercepts systemctl stop / kill

    Logger::instance().init("server.log", LogFormat::JSON);
    LOG_INFO(-1, "HTTP Server initializing on port 8080");

    TcpServer server(8080);
    g_server = &server;

    server.get("/", [](const HttpRequest& req)
    {
        HttpResponse res;
        return res.html("<h1>Hello from C++ HTTP Server!</h1>");
    });

    server.get("/api/health", [](const HttpRequest& req) {
        HttpResponse res;
        return res.json("{\"status\":\"ok\",\"uptime\":\"healthy\"}");
    });

    server.post("/api/echo", [](const HttpRequest& req) {
        HttpResponse res;
        std::string req_body(req.body.begin(), req.body.end());
        return res.set_status(200, "OK").set_body("Echo: " + req_body);
    });

    server.put("/api/resource", [](const HttpRequest& req) {
        HttpResponse res;
        std::string body(req.body.begin(), req.body.end());
        if (body.empty()) {
            return res.set_status(400, "Bad Request").json("{\"error\":\"empty body\"}");
        }
        return res.set_status(200, "OK").json("{\"status\":\"updated\",\"received_bytes\":" + std::to_string(body.size()) + "}");
    });

    server.del("/api/resources", [](const HttpRequest& req) {
        HttpResponse res;
        return res.set_status(204, "No Content"); // no body — good test of Content-Length: 0 path
    });

    server.post("/api/echo/large", [](const HttpRequest& req) {
        HttpResponse res;
        // Deliberately larger payload to exercise multi-round EPOLLOUT flush + the
        // last_activity-during-flush bug path we just discussed
        std::string payload(req.body.begin(), req.body.end());
        return res.set_status(200, "OK").set_body(payload); // pure echo, no transform
    });

    // Testing Chunked Handling with Large Endpoints
    server.get("/test/large", [](const HttpRequest& req) {
        HttpResponse res;
        // Build a body large enough to force MAX_CHUCK_SIZE (64KB) to trigger
        // chunked encoding — 200KB of predictable, verifiable content.
        std::string body;
        body.reserve(200 * 1024);
        for (int i = 0; i < 200 * 1024 / 50; ++i)
        {
            // Fixed-width, numbered lines -> easy to verify nothing got
            // corrupted, duplicated, or reordered by the chunk framing.
            char line[64];
            snprintf(line, sizeof(line), "line-%06d-0123456789abcdef\n", i);
            body += line;
        }
        return res.set_status(200, "OK").html(body);
    });

    server.post("/test/echo", [](const HttpRequest& req) {
        HttpResponse res;
        // Echoes back whatever body it received — this is what proves your
        // chunked REQUEST parser correctly reassembled the client's body.
        return res.set_status(200, "OK").set_header("Content-Type", "text/plain")
        .set_body(req.body);
    });

    if (!server.start())
    {   
        LOG_ERROR(-1, "Failed to start server");
        return 1;
    }

    LOG_INFO(-1, "HTTP Server Listetning on port 8080");

    // Blocks here until server.stop() called from signal_handler
    server.run();

    LOG_INFO(-1, "Server fully stopped. Exiting main().");
    
    //  Stop Logger last after all server resources and worker threads are joined
    Logger::instance().stop();

    return 0;
}