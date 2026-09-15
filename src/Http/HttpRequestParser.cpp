#include "Http/HttpRequestParser.h"

#include <string_view>

ParseResult HttpRequestParser::parse(std::vector<uint8_t>& readBuffer, HttpRequest& request)
{
    while (m_state != State::Complete && m_state != State::Error)
    {
        // Content Length
        if (m_state == State::Body)
        {
            size_t content_length = request.headers.getContentLength();
            size_t bytes_needed = content_length - request.body.size();

            FrameStatus status = Framer::extractBytes(readBuffer, bytes_needed, request.body);

            if (status == FrameStatus::Incomplete) return ParseResult::Incomplete;

            if (status == FrameStatus::Success)
            {
                m_state = State::Complete;
                break;
            }
            else
            {
                // PayloadTooLarge
                m_state = State::Error;
                return ParseResult::Error;
            }
        }

        // Transfer Encoding Chunked
        if (m_state == State::ChunkSize)
        {
            std::string line;
            FrameStatus status = Framer::extractLine(readBuffer, line);
            if (status == FrameStatus::Incomplete) return ParseResult::Incomplete;
            if (status != FrameStatus::Success) { 
                m_state = State::Error; 
                return ParseResult::Error; 
            }

            // Strip chunk extensions if present: "1a;ext=val" -> "1a"
            ssize_t semi = line.find(';');
            std::string size_hex = (semi == std::string::npos) ? line : line.substr(0, semi);

            if (size_hex.empty())
            {
                m_state = State::Error;
                return ParseResult::Error;
            }

            // strtoul quietly accepts a leading '-' (wraps to a huge unsigned value)
            // and leading whitespace before digits - neither is valid chunk-size
            // syntax, so reject up front before even calling strtoul.
            if (size_hex[0] == '-' || size_hex[0] == '+' ||
                std::isspace(static_cast<unsigned char>(size_hex[0])))
            {
                m_state = State::Error;
                return ParseResult::Error;
            }

            errno = 0; // strtoul never clears errno itself - must reset before every call
            char* end = nullptr;
            unsigned long size = std::strtoul(size_hex.c_str(), &end, 16);
            
            // end == c_str():   no digits were consumed at all -> not valid hex
            // *end != '\0':      trailing junk after the hex digits
            // errno == ERANGE:   value too large to fit unsigned long -> overflow
            if (end == size_hex.c_str() || *end != '\0' || errno == ERANGE)
            {
                m_state = State::Error;
                return ParseResult::Error;
            }

            // Reject a single chunk bigger than the entire allowed body up front,
            // before it even gets a chance to accumulate toward the running total.
            if (size > MAX_HTTP_BODY_SIZE)
            {
                m_state = State::Error;
                return ParseResult::Error;
            }

            if (request.body.size() + size > MAX_HTTP_BODY_SIZE)
            {
                m_state = State::Error;
                return ParseResult::Error;
            }

            if (size == 0)
            {
                m_state = State::Trailers;
            }
            else
            {
                m_chunk_remaining = size;
                m_state = State::ChunkData;
            }
            continue;
        }

        if (m_state == State::ChunkData)
        {
            std::vector<uint8_t> chunk;
            FrameStatus status = Framer::extractBytes(readBuffer, m_chunk_remaining, chunk);

            if (status == FrameStatus::Incomplete) return ParseResult::Incomplete;
            if (status != FrameStatus::Success) { 
                m_state = State::Error; 
                return ParseResult::Error; 
            }

            request.body.insert(request.body.end(), chunk.begin(), chunk.end());
            m_state = State::ChunkCRLF;
            continue;
        }

        if (m_state == State::ChunkCRLF)
        {
            std::string line; // must be the empty CRLF terminator after chunk data
            FrameStatus status = Framer::extractLine(readBuffer, line);
            if (status == FrameStatus::Incomplete) return ParseResult::Incomplete;
            if (status != FrameStatus::Success || !line.empty())
            {
                m_state = State::Error;
                return ParseResult::Error;
            }
            m_state = State::ChunkSize; // loop back for next chunk
            continue;
        }

        if (m_state == State::Trailers)
        {
            std::string line;
            FrameStatus status = Framer::extractLine(readBuffer, line);
            if (status == FrameStatus::Incomplete) return ParseResult::Incomplete;
            if (status != FrameStatus::Success) { 
                m_state = State::Error; 
                return ParseResult::Error; 
            }

            if (line.empty()) 
            { 
                m_state = State::Complete; 
                break; 
            } // trailers end
            // else: ignore/discard trailer header line, keep reading
            continue;
        }

        std::string line;
        FrameStatus status = Framer::extractLine(readBuffer, line);

        // If Framer says Incomplete, we pause the FSM and wait for next recv()
        if (status == FrameStatus::Incomplete) return ParseResult::Incomplete;
        // If header is maliciously large, reject it
        if (status != FrameStatus::Success) {
            m_state = State::Error;
            return ParseResult::Error;
        }

        // === PHASE 1: Request Line ===
        if (m_state == State::RequestLine)
        {
            // if Request line is malformed, fail parse
            if (!parseRequestLine(line, request))
            {
                m_state = State::Error;
                return ParseResult::Error;
            }
            // Transition to Headers phase
            m_state = State::Headers;
        }
        // === PHASE 2: Headers ===
        else if (m_state == State::Headers)
        {
            // An empty line signals the end of the HTTP headers block (\r\n\r\n)
            if (line.empty())
            {
                auto te = request.headers.get("Transfer-Encoding");
                if (te.has_value() && te->find("chunked") != std::string::npos)
                {
                    m_state = State::ChunkSize;
                }
                else if (request.headers.getContentLength() > 0)
                {
                    m_state = State::Body;
                }
                else 
                {
                    m_state = State::Complete; //  Requests with no body
                }
            }
            else if (!parseHeaderLine(line, request))
            {
                m_state = State::Error;
                return ParseResult::Error;
            }
        }
    }

    return (m_state == State::Complete) ? ParseResult::Success : ParseResult::Error;
}

bool HttpRequestParser::parseRequestLine(const std::string& line, HttpRequest& request)
{
    //  non-owning reference to a string.
    //  It prevents expensive memory allocations/copies while we slice strings.
    std::string_view sv(line);

    // std::string_view::npos means "No Position" (the character wasn't found)
    size_t first_space = sv.find(' ');
    if (first_space == std::string_view::npos) return false;

    size_t second_space = sv.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) return false;

    std::string_view method_str = sv.substr(0, first_space);
    std::string_view path_str = sv.substr(first_space + 1, second_space - first_space - 1);
    std::string_view version_str = sv.substr(second_space + 1);

    request.method = stringToMethod(std::string(method_str));
    request.path = std::string(path_str);
    request.version = std::string(version_str);

    return request.method != HttpMethod::UNKNOWN && !request.path.empty();
}

bool HttpRequestParser::parseHeaderLine(const std::string& line, HttpRequest& request)
{
    std::string_view vs(line);
    size_t colon_pos = vs.find(':');

    if (colon_pos == std::string_view::npos) {
        return false; // Malformed header line
    }
    
    std::string_view key = vs.substr(0, colon_pos);
    std::string_view value = vs.substr(colon_pos + 1);

    // HTTP specs allow optional whitespace after the colon (e.g., "Key: Value" vs "Key:Value").
    // We trim leading spaces and tabs from the value string_view.
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
    {
        value.remove_prefix(1); // Shifts the start of the view right by 1 character
    }

    // Convert string_view back to std::string for storage
    request.headers.set(std::string(key), std::string(value));
    return true;
}

HttpMethod HttpRequestParser::stringToMethod(const std::string& methodStr)
{
    if (methodStr == "GET") return HttpMethod::GET;
    if (methodStr == "POST") return HttpMethod::POST;
    if (methodStr == "PUT") return HttpMethod::PUT;
    if (methodStr == "DELETE") return HttpMethod::DELETE;
    if (methodStr == "HEAD") return HttpMethod::HEAD;
    if (methodStr == "OPTIONS") return HttpMethod::OPTIONS;
    return HttpMethod::UNKNOWN;
}

void HttpRequestParser::reset()
{
    m_state = State::RequestLine;
    m_chunk_remaining = 0;
}