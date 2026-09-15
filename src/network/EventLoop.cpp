#include "network/EventLoop.h"
#include "network/SocketUtils.h"

#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cerrno>

EventLoop::EventLoop(int serverFD, ThreadPool &pool, const HttpRouter& app_router) : 
            server_fd(serverFD), 
            thread_pool(pool),
            router(app_router)
{
}

EventLoop::~EventLoop() {
    stop();
}

bool EventLoop::start() {
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        LOG_ERROR(epoll_fd, "Failed to create epoll instance.");
        return false;
    }

    // Initialize wakeup_fd
    wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd < 0) {
        LOG_ERROR(-1, "Failed to create eventfd wakeup descriptor.");
        return false;
    }

    // Add wakeup_fd to epoll
    // - EFD_NONBLOCK: Prevents read/write calls on wakeup_fd from blocking the thread.
    // - EFD_CLOEXEC: Automatically closes this descriptor if the process executes a new program (execve).
    struct epoll_event wake_ev{};
    wake_ev.events = EPOLLIN;
    wake_ev.data.fd = wakeup_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wakeup_fd, &wake_ev) < 0) {
        LOG_ERROR(wakeup_fd, "Failed to add wakeup_fd to epoll.");
        return false;
    }

    // Register server_fd for non-blocking Edge-Triggered reads:
    // - EPOLLIN: Notify when there are incoming connections ready to accept.
    // - EPOLLET: Enable Edge-Triggered mode (only notifies once per state change).
    struct epoll_event ev {};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        LOG_ERROR(server_fd, "Failed to add server_fd to epoll.");
        return false;
    }

    return true;
}

void EventLoop::run() {
    state.store(LoopState::RUNNING);
    last_timeout_check = std::chrono::steady_clock::now();

    struct epoll_event events[MAX_EVENTS];

    while (state.load() != LoopState::STOPPED) {

        // if STOPPING phase, stop accepting new connections
        if (state.load() == LoopState::STOPPING && server_fd >= 0)
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, nullptr);
            close(server_fd);
            server_fd = -1;
            LOG_INFO(-1, "Listening socket closed. Draining active connections...");
        }

        // Wait for active read/write events
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 500); // 500ms timeout for graceful exit
        if (nfds < 0) {
            if (errno == EINTR) continue; // Interrupted by signal
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int current_fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            // handle wakeup Trigger
            if (current_fd == wakeup_fd)
            {
                uint64_t dummy;
                ::read(wakeup_fd, &dummy, sizeof(dummy));
                continue;
            }

            // New incoming connections ready to accept
            if (current_fd == server_fd)
            {
                if (state.load() == LoopState::RUNNING)
                {
                    handle_accept();
                }
            }
            // Client Socket Activity
            else
            {
                if (ev & (EPOLLHUP | EPOLLERR))
                {
                    remove_connection(current_fd);
                    continue;
                }

                if ((ev & EPOLLIN) && state.load() == LoopState::RUNNING)
                {
                    handle_client_read(current_fd);
                }
                
                // Allow EPOLLOUT to keep flushing even while STOPPING
                if (ev & EPOLLOUT)
                {
                    handle_client_write(current_fd);
                }
            }
        }

        // Check if shutdown complete during STOPPING state
        if (state.load() == LoopState::STOPPING)
        {
            // Check if all client sockets finished writing and closed
            bool no_active_connections = false;
            {
                std::lock_guard<std::mutex> lock(conn_map_mtx);
                no_active_connections = connections.empty();
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - shutdown_start);

            // Exit conditions: connections finished OR timeout reached
            if (no_active_connections || elapsed >= SHUTDOWN_TIMEOUT)
            {
                if (!no_active_connections)
                {
                    LOG_WARN(-1, "Shutdown timeout reached. Force-closing remaining sockets.");
                }
                state.store(LoopState::STOPPED);
            }
        }

        // Periodically purge silent/abandoned sockets
        check_idle_timeouts();
    }

    // Cleanup phase after exiting loop
    cleanup_all_connections();
    if (wakeup_fd >= 0) {
        close(wakeup_fd);
        wakeup_fd = -1;
    }
    LOG_INFO(-1, "EventLoop shutdown complete. Exiting run().");
}

void EventLoop::handle_accept()
{
    while (state.load() == LoopState::RUNNING)
    {

        // Global capacity check if full stop draining the kernel's queue
        // leftover connections stay queued at tcp level until
        // remove_connection() re-triggers this function once a slot frees.
        if (active_connections >= MAX_TOTAL_CONNECTIONS)
        {
            break;
        }

        struct sockaddr_in client_addr {};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, reinterpret_cast<struct sockaddr*> (&client_addr), &client_len);
        if (client_fd < 0) 
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // all pending connections accepted normal exit
            if (errno == ECONNABORTED || errno == EINTR || errno == EPROTO)
            {
                LOG_WARN(server_fd, "Transient accept() error, continuing");
                continue; // try the next pending connection
            }
            // EMFILE, ENFILE, ENOBUFS, ENOMEM - fd exhaustion / resource limits, genuinely fatal
            LOG_ERROR(server_fd, "Fatal accept() error, stopping accept loop");
            break;
        }

        // Extracting ip address
        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_buf, sizeof(ip_buf));
        std::string ip(ip_buf);

        // Per-IP Connection cap check
        if (!ip_conn_limiter.try_acquire(ip))
        {
            LOG_WARN(client_fd, "Connection rejected - too many open connections from " + ip);
            close(client_fd); // reject immediately, no Connection object, no epoll registration
            continue;
        }

        SocketUtils::set_non_blocking(client_fd);

        // Create a Connection instance for new socket
        auto conn = std::make_shared<Connection>(client_fd);
        conn->set_client_ip(ip_buf);
        {
            std::lock_guard<std::mutex> lock(conn_map_mtx);
            connections[client_fd] = conn;
        }

        // Register client with EPOLLONESHOT to prevent multithreaded races across worker threads
        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET |EPOLLONESHOT;
        ev.data.fd = client_fd;

        // epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) 
        {
            LOG_ERROR(client_fd, "Failed to add client_fd to epoll");
            ip_conn_limiter.release(ip); // undo the aquire since this connection never took hold
            {
                std::lock_guard<std::mutex> lock(conn_map_mtx);
                connections.erase(client_fd);
            }
            close(client_fd);
            continue;
        }

        active_connections.fetch_add(1, std::memory_order_relaxed); // only count fully registered connections
    }
}

void EventLoop::handle_client_read(int client_fd)
{
    if (state.load() != LoopState::RUNNING) return;

    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(conn_map_mtx);
        auto it = connections.find(client_fd);
        if (it == connections.end()) return;
        conn = it->second;
    }

    thread_pool.submit([this, conn, client_fd] 
    {
        bool close_connections = false;
        bool has_error = false;

        try
        {
            std::lock_guard<std::mutex> lock(conn->get_mutex());

            // 1. Drain Socket Ingress Buffer
            uint8_t temp_buf[4096];
            while (state.load() == LoopState::RUNNING)
            {
                LOG_INFO(client_fd, "[TID: " + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + "] Starting recv pass");
                ssize_t received_bytes = recv(client_fd, temp_buf, sizeof(temp_buf), MSG_DONTWAIT);
                if (received_bytes > 0)
                {
                    conn->append_read_data(temp_buf, received_bytes);
                }
                else if (received_bytes == 0)
                {
                    close_connections = true;
                    break;
                }
                else
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    LOG_ERROR(client_fd, "recv() failed, errno: " + std::to_string(errno));
                    close_connections = true;
                    break;
                }
            }

            if (close_connections) goto cleanup;

            // 2. Parse Loop Execution
            auto& read_buffer = conn->get_read_buffer();
            auto& parser = conn->get_parser();
            auto& req = conn->get_request();

            // State machine for handling persisent connections
            size_t loop_counter = 0;
            bool has_more_parseable = false;
            bool close_after_response = false; 

            while (!read_buffer.empty())
            {
                // Initialize RAII Logger for this request
                loop_counter++;
                
                {
                    HttpLogGuard log_guard(client_fd, req);

                    ParseResult p_result = parser.parse(read_buffer, req);

                    if (p_result == ParseResult::Incomplete) break;

                    if (p_result == ParseResult::Error)
                    {
                        HttpResponse err_res;
                        err_res.set_status(400, "Bad Request").html("<h1>400 Bad Request</h1>");
                        err_res.finalize_for_connection(false, false);
                        
                        std::vector<uint8_t> wire_data = HttpResponseEncoder::encode(err_res);
                        log_guard.set_response(err_res);
                        conn->append_unsent_data(wire_data.data(), wire_data.size());

                        conn->flush(has_error);
                        close_connections = true;
                        goto cleanup;
                    }

                    if (p_result == ParseResult::Success)
                    {
                        // Raate limit check, before doing any real work for this request
                        if (!rate_limiter.allow(conn->get_client_ip()))
                        {
                            HttpResponse limit_res;
                            limit_res.set_status(429, "Too Many Requests")
                                        .set_header("Retry-After", "1")
                                        .html("<h1>429 Too Many Requests</h1>");
                            limit_res.finalize_for_connection(false, false); // false = force connection: close

                            std::vector<uint8_t> wire_data = HttpResponseEncoder::encode(limit_res);
                            log_guard.set_response(limit_res);
                            conn->append_unsent_data(wire_data.data(), wire_data.size());

                            conn->flush(has_error);
                            close_connections = true;
                            goto cleanup;
                        }

                        bool keep_alive = req.wants_keep_alive();
                        bool is_head = (req.method == HttpMethod::HEAD);

                        HttpResponse res = router.route(req);
                        res.finalize_for_connection(keep_alive, is_head);

                        if (res.close_after_send) close_after_response = true;
                        
                        std::vector<uint8_t> wire_bytes = HttpResponseEncoder::encode(res);
                        log_guard.set_response(res);
                        conn->append_unsent_data(wire_bytes.data(), wire_bytes.size());
                    }
                } // log_guard is destroyed here

                req.reset();
                parser.reset();

                // Stop pipelining once we've committed to closing — no point parsing
                // a next request the client may still send before seeing our FIN.
                if (close_after_response) break;

                if (loop_counter > 50) {
                    LOG_ERROR(client_fd, "Infinite parse loop detected! Breaking out.");
                    has_more_parseable = true;
                    break;
                }
            }

            // 3. Flush Phase
            bool fully_flushed = conn->flush(has_error);
            if (has_error) {
                close_connections = true;
                goto cleanup;
            }

            if (close_after_response)
            {
                if (fully_flushed)
                {
                     // everything sent, safe to tear down now
                    close_connections = true;
                    goto cleanup;
                }
                else {
                    // Bytes still queued — must keep writing before closing.
                    // Mark it so handle_client_write closes instead of re-arming EPOLLIN
                    // once the buffer finally drains.
                    conn->mark_close_after_write();
                    modify_epoll(client_fd, EPOLLOUT | EPOLLET | EPOLLONESHOT);
                    return; // skip the normal re-arm logic below
                }
            }

            // 4. Epoll Re-arm
            uint32_t flags = EPOLLIN | EPOLLET | EPOLLONESHOT;
            if (!fully_flushed || conn->has_pending_writes()) {
                flags |= EPOLLOUT;
            }
            modify_epoll(client_fd, flags);


            if (has_more_parseable && state.load() == LoopState::RUNNING) {
                thread_pool.submit([this, client_fd]() { handle_client_read(client_fd); });
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR(client_fd, std::string("Unhandled Execption: ") + e.what());
            close_connections = true;
        }
        catch (...)
        {
            LOG_ERROR(client_fd, "Fatal unknown exception occurred during worker handling!");
            close_connections = true;
        }

    cleanup:
        if (close_connections || has_error)
        {
            remove_connection(client_fd);
        }
    });
}

void EventLoop::handle_client_write(int client_fd)
{
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(conn_map_mtx);
        auto it = connections.find(client_fd);
        if (it == connections.end()) return;
        conn = it->second;
    }

    thread_pool.submit([this, conn, client_fd] 
    {
        bool has_error { false };
        bool fully_flushed { false };
        bool should_close = false;
        
        {
            std::lock_guard<std::mutex> lock(conn->get_mutex());
            fully_flushed = conn->flush(has_error);
            if (fully_flushed) should_close = conn->should_close_after_write();
        }

        if (has_error)
        {
            remove_connection(client_fd);
            return;
        }

        if (should_close)
        {
            remove_connection(client_fd); 
            return; 
        }

        if (fully_flushed)
        {
            if (state.load() == LoopState::RUNNING)
            {
                // Buffer Empty Remove EPOLLOUT tp prevent unecessary event notifications
                // Return to read-only state if server is still running
                modify_epoll(client_fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
            }
            else
            {
                // If Stopping and flush is complete, close connection
                remove_connection(client_fd);
            }
        }
        else
        {
            // Unsent bytes remain; continue listening for write readiness
            uint32_t flags = EPOLLOUT | EPOLLET | EPOLLONESHOT;
            if (state.load() == LoopState::RUNNING) flags |= EPOLLIN;
            modify_epoll(client_fd, flags);
        }
    });
}

void EventLoop::modify_epoll(int client_fd, uint32_t events)
{
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = client_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
}

void EventLoop::remove_connection(int client_fd) {
    std::shared_ptr<Connection> conn;
    {
        std::lock_guard<std::mutex> lock(conn_map_mtx);
        auto it = connections.find(client_fd);
        if (it == connections.end()) return;
        conn = it->second;
        connections.erase(it);
    }

    ip_conn_limiter.release(conn->get_client_ip()); // Release the slot for this IP
    active_connections.fetch_sub(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(conn->get_mutex()); // wait out any in-flight worker
    if (epoll_fd >= 0) epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    // conn destructs here (or when last shared_ptr drops) and closes the fd

    // Edge-triggered listening socket: if we were previously at capacity and
    // stopped draining the accept queue, this slot freeing up is the only
    // chance to resume draining it - epoll won't re-notify us on its own.
    if (state.load() == LoopState::RUNNING && server_fd >= 0)
    {
        handle_accept();
    }
}

void EventLoop::check_idle_timeouts()
{
    auto now = std::chrono::steady_clock::now();

    // ONly Run Check Scan after 5 Minutes
    if (std::chrono::duration<double>(now - last_timeout_check).count() < CHECK_INTERVAL_SECONDS)
    {
        return;
    }

    last_timeout_check = now;

    // existing connection-timeout sweep
    std::vector<int> time_out_fds;
    {
        std::lock_guard<std::mutex> lock(conn_map_mtx);
        for(const auto& [fd, conn] : connections)
        {
            if (conn->seconds_since_last_activity() >= IDLE_TIMEOUT_SECONDS)
            {
                time_out_fds.push_back(fd);
            }
        }
    }

    // Remove timed-out connections
    for(int fd: time_out_fds)
    {
        remove_connection(fd);
    }

    // rate limiter bucket cleanup, same cadence, piggybacking on this
    // periodic tick rather than needing its own timer/thread
    rate_limiter.sweep_stale();
}

bool EventLoop::has_pending_writes()
{
    std::lock_guard<std::mutex> lock(conn_map_mtx);
    for (const auto& [fd, conn] : connections)
    {
        std::lock_guard<std::mutex> lock(conn->get_mutex());
        if (conn->has_pending_writes())
        {
            return true;
        }
    }
    return false;
}

void EventLoop::cleanup_all_connections()
{
    std::lock_guard<std::mutex> lock(conn_map_mtx);
    for (auto& [fd, conn] : connections) {
        ip_conn_limiter.release(conn->get_client_ip()); // release each one 
        active_connections.fetch_sub(1, std::memory_order_relaxed);
        if (epoll_fd >= 0) epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
    }
    connections.clear();

    if (epoll_fd >= 0) {
        close(epoll_fd);
        epoll_fd = -1;
    }
}

void EventLoop::stop()
{
    // Prevent Multi access to stopping
    LoopState expected = LoopState::RUNNING;
    if (!state.compare_exchange_strong(expected, LoopState::STOPPING))
    {
        return; // Already stopping or stopped
    }

    shutdown_start = std::chrono::steady_clock::now();

    // Wake up epoll_wait immediately if it's sleeping
    if (wakeup_fd >= 0) {
        uint64_t one = 1;
        ::write(wakeup_fd, &one, sizeof(one));
    }
}
