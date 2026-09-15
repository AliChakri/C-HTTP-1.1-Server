#include "Http/HttpResponseEncoder.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

// Helper lambda to append raw string characters directly into vector<uint8_t>
static void appendString(std::vector<uint8_t>& dest, const std::string& str) {
    dest.insert(dest.end(), str.begin(), str.end());
}

std::vector<uint8_t> HttpResponseEncoder::encode(HttpResponse& response, bool is_head_request)
{
    std::vector<uint8_t> wire_data;

    // Pre-Allocate buffer space tp prevent memory reallocation during formating
    wire_data.reserve(256 + response.body.size());

    // 1. Format Status Line directly into vector: "HTTP/1.1 200 OK\r\n"
    appendString(wire_data, "HTTP/1.1 ");
    appendString(wire_data, std::to_string(response.status_code));
    appendString(wire_data, " ");
    appendString(wire_data, response.status_message);
    appendString(wire_data, "\r\n");

    // Handle Encoding Format
    set_response_header(response);

    // 2. Format Headers directly into vector
    for (const auto& [key, value]: response.headers.getAll())
    {
        appendString(wire_data, key);
        appendString(wire_data, ": ");
        appendString(wire_data, value);
        appendString(wire_data, "\r\n");

        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
    }

    // 4 Header-Body delimiter line
    appendString(wire_data, "\r\n");


    if (!is_head_request)
    {
        // 5. Append raw binary/text body bytes
        if (response.body_type == BodyType::CONTENT_LENGTH)
        {
            // Content Length Encoding
            if (!response.body.empty())
            {
                wire_data.insert(wire_data.end(), response.body.begin(), response.body.end());
            }
        }
        // Transfer Encoding CHUNCKED
        else
        {
            size_t body_size = response.body.size();
            size_t offset = 0;

            while (offset < body_size)
            {
                size_t chunk_len = std::min(MAX_CHUCK_SIZE, body_size - offset);

                // chunk-size line, hex, no leading zeros, CRLF
                std::ostringstream size_line;
                size_line << std::hex << chunk_len;
                appendString(wire_data, size_line.str());
                appendString(wire_data, "\r\n");

                // chunk-data + CRLF
                wire_data.insert(wire_data.end(),
                                    response.body.begin() + offset,
                                    response.body.begin() + offset + chunk_len);

                appendString(wire_data, "\r\n");

                offset += chunk_len;
            }
            
            // terminating chunk (0-length) + empty trailer + final CRLF
            appendString(wire_data, "0\r\n\r\n");
        }    
    }

    return wire_data;
}

void HttpResponseEncoder::set_response_header(HttpResponse& res)
{
    size_t body_size = res.body.size();
    if (body_size > MAX_CHUCK_SIZE)
    {
        res.body_type = BodyType::CHUNKED;
        res.headers.del("Content-Length"); 
        res.headers.set("Transfer-Encoding", "chunked");   
    }
    else if (body_size == 0)
    {
        res.headers.set("Content-Length", "0");
    }
    else
    {
        res.body_type = BodyType::CONTENT_LENGTH;
        res.headers.del("Transfer-Encoding"); 
        res.headers.set("Content-Length", std::to_string(body_size));
    }
}