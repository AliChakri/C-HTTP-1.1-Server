#pragma once

#include "Http/HttpRequest.h"
#include "Http/HttpRequestParser.h"
#include "Utils/Logger.h"

#include <vector>
#include <mutex>
#include <chrono>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>

class Connection 
{
private:
    int fd;
    std::string client_ip;
    std::vector<uint8_t> read_buffer;  // Raw network ingress stream
    std::vector<uint8_t> write_buffer; // Raw network egress stream
    std::mutex conn_mutex;        // Protects buffers against worker thread races
    
    bool close_after_write_flag{false};

    std::chrono::steady_clock::time_point last_activity;

    // HTTP State per Connection
    HttpRequestParser parser;
    HttpRequest request;


public:
    explicit Connection(int client_fd) : fd(client_fd), last_activity(std::chrono::steady_clock::now()) {}
    ~Connection();

    int get_fd() const;
    void set_client_ip(std::string ip) { client_ip = std::move(ip); }
    const std::string& get_client_ip() const { return client_ip; }
    
    std::vector<uint8_t>& get_read_buffer();
    std::mutex& get_mutex();

    // Accessors for HTTP Processing
    HttpRequestParser& get_parser() { return parser; }
    HttpRequest& get_request() { return request; }

    // Call this whenever recv() or send() succeeds on this socket
    void update_activity();
    // Returns seconds passed since last I/O activity
    double seconds_since_last_activity();

    void append_unsent_data(const uint8_t* data, size_t length);
    void append_read_data(const uint8_t* data, size_t length);

    bool flush(bool& out_error);    
    bool has_pending_writes();

    // State checking for persisnet connections
    void mark_close_after_write() { close_after_write_flag = true; }
    bool should_close_after_write() const { return close_after_write_flag; }
};