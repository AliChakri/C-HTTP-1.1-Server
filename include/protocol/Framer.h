#pragma once

#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <arpa/inet.h>

constexpr size_t MAX_HTTP_HEADER_SIZE =  8 * 1024; // 8 KB for headers (prevent memory attacks)
constexpr size_t MAX_HTTP_BODY_SIZE = 10 * 1024 * 1024; // 10MB max Body size
constexpr size_t MAX_CHUNK_BODY_SIZE = 64 * 1024; // 64KB Max Chunck Size

enum class FrameStatus
{
    Success,
    Incomplete,
    HeaderTooLarge,
    PayloadTooLarge
};

class Framer {
public:
    // Scans readBuffer for next CRLF ("\r\n").
    // If found: extracts the line (without \r\n) and erases it from readBuffer.
    // Enforces MAX_HTTP_HEADER_SIZE limit.
    static FrameStatus extractLine(std::vector<uint8_t>& readBuffer, std::string& outLine);

    // Checks if readBuffer has at least `requiredBytes`.
    // If available: extracts the byte slice into outBytes and erases them from readBuffer.
    // Enforces MAX_HTTP_BODY_SIZE limit.
    static FrameStatus extractBytes(std::vector<uint8_t>& readBuffer, size_t requiredBytes, std::vector<uint8_t>& outBytes);
};