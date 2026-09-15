#include "Http/HttpResponse.h"

#include <algorithm>

HttpResponse& HttpResponse::set_status(uint16_t code, std::string message)
{
    status_code = code;
    status_message = std::move(message);
    return *this;
}

HttpResponse& HttpResponse::set_header(std::string key, std::string value)
{
    headers.set(std::move(key), std::move(value));
    return *this;
}

HttpResponse& HttpResponse::set_body(std::string body_text)
{
    body.assign(body_text.begin(), body_text.end());
    return *this;
}

HttpResponse& HttpResponse::set_body(std::vector<uint8_t> body_bytes)
{
    body = std::move(body_bytes);
    return *this;
}

HttpResponse& HttpResponse::json(const std::string& json_str)
{
    set_header("Content-Type", "application/json");
    set_body(json_str);
    return *this;
}

HttpResponse& HttpResponse::html(const std::string& html_str)
{
    set_header("Content-Type", "text/html; charset=utf-8");
    set_body(html_str);
    return *this;
}

void HttpResponse::finalize_for_connection(bool client_wants_keep_alive, bool is_head_request)
{
    // A handler may have already forced a close (e.g. 500s, or it explicitly
    // set Connection: close itself) — respect that; don't downgrade it.
    bool already_close = false;
    auto existing = headers.get("Connection");
    if (existing.has_value())
    {
        std::string v = *existing;
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        already_close = (v.find("close") != std::string::npos);
    }

    bool keep_alive = client_wants_keep_alive && !already_close;
    set_header("Connection", keep_alive ? "keep-alive" : "close");
    close_after_send = !keep_alive;
}

void HttpResponse::reset()
{
    status_code = 200;
    status_message = "OK";
    headers = HttpHeaders();
    body.clear();
}