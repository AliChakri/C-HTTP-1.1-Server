#include "Utils/Logger.h"

Logger& Logger::instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger() : running(false) {}

Logger::~Logger() 
{
    stop();
}

void Logger::init(const std::string& file_path, LogFormat fmt)
{
    std::lock_guard<std::mutex> lock(queue_mtx);
    if (running) return;

    format = fmt;
    log_file_path = file_path;
    log_file.open(log_file_path, std::ios::out | std::ios::app);
    running = true;
    worker_thread = std::thread(&Logger::process_queue, this);
}

void Logger::stop()
{
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        if (!running) return;
        running = false;
    }
    cv.notify_all();
    if (worker_thread.joinable())
    {
        worker_thread.join();
    }

    if (log_file.is_open())
    {
        log_file.close();
    }
}

void Logger::log(Level level, int fd, const std::string& msg)
{
    Logger& logger = instance();
    if (!logger.running) return;

    LogEntry entry
    {
        level, msg, fd, 
        std::this_thread::get_id(), 
        std::chrono::system_clock::now()
    };
    {
        std::lock_guard<std::mutex> lock(logger.queue_mtx);
        logger.queue.push(entry);
    }
    logger.cv.notify_one();
}

void Logger::log_http(int fd, const std::string& method, const std::string& path, int status, double duration_ms)
{
   Logger& logger = instance();
   if (!logger.running) return;
   
   Level level = Level::INFO;
   if (status >= 400 && status < 500) level = Level::WARN;
   if (status >= 500 || status == 0) level = Level::ERROR;

   std::stringstream ss;
   ss << "\"" << method << " " << path << "\" " << status << " - " << std::fixed << std::setprecision(2) << duration_ms << "ms";

   LogEntry entry;
   entry.level = level;
   entry.message = ss.str();
   entry.fd = fd;
   entry.thread_id = std::this_thread::get_id();
   entry.timestamp = std::chrono::system_clock::now();

    entry.method = method;
    entry.path = path;
    entry.status_code = status;
    entry.duration_ms = duration_ms;

    {
        std::lock_guard<std::mutex> lock(logger.queue_mtx);
        logger.queue.push(entry);
    }
    logger.cv.notify_one();
}

void Logger::trace_lock(int fd, const std::string& mutex_name, const std::string& action)
{
    std::stringstream ss;
    ss << "[MUTEX] " << action << " " << mutex_name;
    log(Level::DEBUG, fd, ss.str());
}

std::string Logger::escape_json(const std::string& input)
{
    std::stringstream ss;
    for(char c: input)
    {
        switch (c)
        {
            case '"': ss << "\\\""; break;
            case '\\': ss << "\\\\"; break;
            case '\b': ss << "\\b"; break;
            case '\f': ss << "\\f"; break;
            case '\n': ss << "\\n"; break;
            case '\e': ss << "\\e"; break;
            case '\r': ss << "\\r"; break;
            case '\t': ss << "\\t"; break;
            default: ss << c; break;
        }
    }

    return ss.str();
}

std::string Logger::format_text(const LogEntry& entry)
{
    auto in_time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (
        entry.timestamp.time_since_epoch()) % 1000;
    
    const char* lvl_str = "INFO";
    switch (entry.level) {
        case Level::DEBUG: lvl_str = "DEBUG"; break;
        case Level::INFO:  lvl_str = "INFO "; break;
        case Level::WARN:  lvl_str = "WARN "; break;
        case Level::ERROR: lvl_str = "ERROR"; break;
    }

    std::stringstream ss;
    ss << "["   << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S")
                << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
                << "[" << lvl_str << "] "
                << "[TID " << entry.thread_id << "] ";
            
    if (entry.fd >= 0)
    {
        ss << "[FD " << entry.fd << "] ";
    }

    ss << entry.message;
    return ss.str();
}

std::string Logger::format_json(const LogEntry& entry)
{
    auto in_time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (
        entry.timestamp.time_since_epoch()) % 1000;

    const char* lvl_str = "INFO";
    switch (entry.level) {
        case Level::DEBUG: lvl_str = "DEBUG"; break;
        case Level::INFO:  lvl_str = "INFO "; break;
        case Level::WARN:  lvl_str = "WARN "; break;
        case Level::ERROR: lvl_str = "ERROR"; break;
    }

    std::stringstream ss;
    ss << "{";

    ss  << "\"timestamp\":\"" << std::put_time(std::gmtime(&in_time_t),"%Y-%m-%d %H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z\",";
    
    ss << "\"level\":\"" << lvl_str << "\",";

    std::stringstream tid_ss;
    tid_ss << entry.thread_id;
    ss << "\"tid\":\"" << tid_ss.str() << "\",";

    if (entry.fd >= 0)
    {
        ss << "\"fd\":" << entry.fd << ",";
    }

    if (!entry.method.empty())
    {
        ss << "\"method\":\"" << escape_json(entry.method) << "\",";
        ss << "\"path\":\"" << escape_json(entry.path) << "\",";
        ss << "\"status\":" << entry.status_code << ",";
        ss << "\"duration_ms\":" << std::fixed << std::setprecision(2) << entry.duration_ms << ",";
    }

    ss << "\"message\":\"" << escape_json(entry.message) << "\"";
    ss << "}";

    return ss.str();
}

void Logger::process_queue()
{
    while (true)
    {
        LogEntry entry;
        {
            std::unique_lock<std::mutex> lock(queue_mtx);
            cv.wait(lock, [this](){
                return !queue.empty() || !running;
            });

            if (!running && queue.empty()) break;

            entry = queue.front();
            queue.pop();
        }

        std::string formatted = (format == LogFormat::JSON) ? format_json(entry): format_text(entry);

        std::cout << formatted << std::endl;
        if (log_file.is_open())
        {
            log_file << formatted << "\n";
            log_file.flush();
        }
    }   
}