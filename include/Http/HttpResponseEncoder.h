#pragma once

#include "Http/HttpResponse.h"

#include <vector>
#include <cstdint>

constexpr size_t MAX_CHUCK_SIZE = 64 * 1024; // 64KB Chunck

class HttpResponseEncoder
{
public:
    // Serializes HttpResponse into raw HTTP/1.1 wire-format bytes
    static std::vector<uint8_t> encode(HttpResponse& response, bool is_head_request = false);

    static void set_response_header(HttpResponse& res);
};
