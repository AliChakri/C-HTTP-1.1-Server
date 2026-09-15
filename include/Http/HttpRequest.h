#pragma once

#include "Http/HttpHeaders.h"

#include <vector>
#include <string>
#include <cstdint>

enum class HttpMethod
{
    GET, POST, PUT, DELETE, HEAD, OPTIONS, UNKNOWN
};

class HttpRequest
{
public:
    HttpMethod method { HttpMethod::UNKNOWN };
    std::string path;
    std::string version;
    HttpHeaders headers;
    std::vector<uint8_t> body;

    // Resets fields to reuse object across HTTP Keep-Alive requests
    void reset();
    bool wants_keep_alive() const;
};
