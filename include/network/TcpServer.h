#pragma once

#include "concurrency/ThreadPool.h"
#include "concurrency/TaskQueue.h"
#include "network/EventLoop.h"
#include "network/SocketUtils.h"
#include "Http/HttpRouter.h"
#include "Utils/Logger.h"

#include <sys/socket.h>
#include <string>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

class TcpServer
{
private:
    int server_fd { -1 };
    uint16_t port;

    bool is_running { false };

    HttpRouter router; // Shared Application Router instance
    TaskQueue task_queue;
    ThreadPool thread_pool;
    std::unique_ptr<EventLoop> event_loop;

    std::mutex counter_mtx;
    static inline uint32_t num_requests{0};

public:
    explicit TcpServer(uint16_t port_num);
    ~TcpServer();

    // Access router to register routes from main()
    HttpRouter& get_router() { return router; }

    // Convenience route-registration wrappers
    void get(const std::string& path, HttpHandler handler) { router.get(path, std::move(handler)); }
    void post(const std::string& path, HttpHandler handler) { router.post(path, std::move(handler)); }
    void put(const std::string& path, HttpHandler handler) { router.put(path, std::move(handler)); }
    void del(const std::string& path, HttpHandler handler) { router.del(path, std::move(handler)); }

    bool start();
    void run();
    void stop();
};