#pragma once

#include "Http/HttpHeaders.h"

#include <vector>
#include <string>
#include <cstdint>

enum class BodyType
{
    CONTENT_LENGTH,     CHUNKED
};

class HttpResponse
{
public:
    uint16_t status_code{200};
    std::string status_message{"OK"};
    HttpHeaders headers;
    std::vector<uint8_t> body;
    BodyType body_type { BodyType::CONTENT_LENGTH };
    bool close_after_send{false};

    HttpResponse() = default;

    // Fluent helper methods for convenience
    HttpResponse& set_status(uint16_t code, std::string message);
    HttpResponse& set_header(std::string key, std::string value);
    HttpResponse& set_body(std::string body_text);
    HttpResponse& set_body(std::vector<uint8_t> body_bytes);
    
    // Common shortcuts
    HttpResponse& json(const std::string& json_str);
    HttpResponse& html(const std::string& html_str);

    void finalize_for_connection(bool client_wants_keep_alive, bool is_head_request);

    // Reset instance for Re-Use
    void reset();
};