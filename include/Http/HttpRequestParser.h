#pragma once

#include "Http/HttpRequest.h"
#include "protocol/Framer.h"

#include <vector>
#include <string>

enum class ParseResult
{
    Success,    // Request fully parsed
    Incomplete, // Needs more TCP data
    Error       // Invalid syntax or payload limits exceeded
};

class HttpRequestParser
{
public:
    enum class State
    {
        RequestLine,
        Headers,
        Body,           // Content-Length path
        ChunkSize,     // chunked path
        ChunkData,
        ChunkCRLF,
        Trailers,
        Complete,
        Error
    };

    HttpRequestParser() = default;

    // Attempts to parse the request from the read buffer.
    ParseResult parse(std::vector<uint8_t>& readBuffer, HttpRequest& request);

    void reset();
    bool isComplete() const { return m_state == State::Complete; }

private:
    State m_state {State::RequestLine};
    size_t m_chunk_remaining {0};

    // Helper Methods
    bool parseRequestLine(const std::string& line, HttpRequest& request);
    bool parseHeaderLine(const std::string& line, HttpRequest& request);
    HttpMethod stringToMethod(const std::string& methodStr);
};