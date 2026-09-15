#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <csignal>

enum class Level
{
    INFO, WARN, ERROR, DEBUG
};

enum class LogFormat
{
    TEXT, JSON
};

struct LogEntry
{
    Level level;
    std::string message;
    int fd;
    std::thread::id thread_id;
    std::chrono::system_clock::time_point timestamp;

    // HTTP Metada Optional
    std::string method;
    std::string path;
    int status_code{0};
    double duration_ms{0.0};
};

class Logger
{
private:
    std::string log_file_path;
    std::ofstream log_file;

    std::queue<LogEntry> queue;
    std::mutex queue_mtx;
    std::condition_variable cv;

    std::thread worker_thread;
    bool running;
    LogFormat format { LogFormat::TEXT };

    Logger();
    ~Logger();

    void process_queue();
    std::string format_text(const LogEntry& entry);
    std::string format_json(const LogEntry& entry);
    std::string escape_json(const std::string& input);
    
public:
    static Logger& instance();

    void init(const std::string& filepath = "server.log", LogFormat fmt = LogFormat::TEXT);
    void stop();

    static void log(Level level, int fd, const std::string& msg);
    static void log_http(int fd, const std::string& method, const std::string& path, int status, double duration_ms);

    // Concurrency debugging helper
    static void trace_lock(int fd, const std::string& mutex_name, const std::string& action);
  
};

// Convenience macros for clean call sites
#define LOG_DEBUG(fd, msg) Logger::log(Level::DEBUG, fd, msg)
#define LOG_INFO(fd, msg)  Logger::log(Level::INFO, fd, msg)
#define LOG_WARN(fd, msg)  Logger::log(Level::WARN, fd, msg)
#define LOG_ERROR(fd, msg) Logger::log(Level::ERROR, fd, msg)
#define LOG_LOCK(fd, mutex_name, action) Logger::trace_lock(fd, mutex_name, action)