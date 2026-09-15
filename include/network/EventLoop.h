#pragma once

#include "concurrency/ThreadPool.h"
#include "network/Connection.h"
#include "protocol/Framer.h"
#include "Http/HttpRouter.h"
#include "Http/HttpResponseEncoder.h"
#include "Utils/HttpLogGuard.h"
#include "middleware/RateLimiter.h"
#include "middleware/PerIpConnectionLimiter.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cerrno>

enum class LoopState
{
    RUNNING, STOPPING, STOPPED
};

class EventLoop
{
private:
    static constexpr int MAX_EVENTS = 256;
    static constexpr double IDLE_TIMEOUT_SECONDS = 30.0; 
    static constexpr double CHECK_INTERVAL_SECONDS = 10.0;
    static constexpr size_t MAX_TOTAL_CONNECTIONS = 10000;

    std::atomic<LoopState> state { LoopState::STOPPED };
    std::chrono::steady_clock::time_point shutdown_start;
    const std::chrono::milliseconds SHUTDOWN_TIMEOUT{5000}; // 5-second max drain

    int epoll_fd { -1 };
    int server_fd { -1 };
    int wakeup_fd{-1}; // eventfd used to break epoll_wait instantly

    ThreadPool& thread_pool;

    const HttpRouter& router; // Store REFRENCE TO APPLICATION ROUTER

    // Rate Limit
    RateLimiter rate_limiter{10.0, 20.0}; // 10 req/sec steady, burst of 20
    PerIpConnectionLimiter ip_conn_limiter{ 100 }; // max 100 concurent connections per IP, tune as needed
    std::atomic<size_t> active_connections{ 0 }; // Keep track of active conection

    std::unordered_map<int, std::shared_ptr<Connection>> connections;
    std::mutex conn_map_mtx; // protects connection map

    std::chrono::steady_clock::time_point last_timeout_check;

    void handle_accept();
    void handle_client_read(int client_fd);
    void handle_client_write(int client_fd);

    // Helper fucntions to update epoll flags
    void modify_epoll(int fd, uint32_t events);
    void remove_connection(int fd);

    void check_idle_timeouts();
    bool has_pending_writes();

    void cleanup_all_connections();

public:
    EventLoop(int server_fd, ThreadPool &pool, const HttpRouter& app_router);
    ~EventLoop();

    bool start();
    void run();
    void stop();
};
