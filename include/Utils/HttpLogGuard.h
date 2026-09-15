#pragma once

#include "Utils/Logger.h"
#include "Http/HttpRequest.h"
#include "Http/HttpResponse.h"

#include <chrono>

class HttpLogGuard
{
private:
    int m_fd;
    const HttpRequest& m_req;
    HttpResponse m_fallback_res;
    const HttpResponse* m_res_ptr;
    std::chrono::steady_clock::time_point m_start_time;

    static std::string method_to_string(HttpMethod method)
    {
        switch (method)
        {
        case HttpMethod::GET :  return "GET";
        case HttpMethod::POST :  return "POST";
        case HttpMethod::PUT :  return "PUT";
        case HttpMethod::DELETE :  return "DELETE";
        case HttpMethod::HEAD :  return "HEAD";
        case HttpMethod::OPTIONS :  return "OPTIONS";
        default:                    return "UNKNOWN";
        }
    }

public:
    HttpLogGuard(int fd, const HttpRequest& req) :
                m_fd(fd), m_req(req), m_res_ptr(&m_fallback_res)
    {
        m_start_time = std::chrono::steady_clock::now();
    }

    void set_response(const HttpResponse& res)
    {
        m_res_ptr = &res;
    }

    ~HttpLogGuard()
    {
        auto end_time = std::chrono::steady_clock::now();
        double duration_ms = std::chrono::duration<double, std::milli> (end_time - m_start_time).count();

        std::string method = method_to_string(m_req.method);
        if (method.empty()) method = "UNKNOWN";

        std::string path = m_req.path.empty() ? "/" : m_req.path;
        uint16_t code = m_res_ptr->status_code;

        Logger::log_http(m_fd, method, path, code, duration_ms);
    }
};