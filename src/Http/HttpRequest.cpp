#include "Http/HttpRequest.h"

#include <algorithm>
#include <cctype>

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

bool HttpRequest::wants_keep_alive() const
{
    // Adjust `headers.get(...)` to whatever HttpHeaders.h actually exposes
    // (case-insensitive lookup is required here — if it's not already
    // case-insensitive internally, this will silently misbehave).
    auto conn = headers.get("Connection");
    if (conn.has_value())
    {
        std::string v = to_lower(*conn);
        if (v.find("close") != std::string::npos)      return false;
        if (v.find("keep-alive") != std::string::npos) return true;
    }
    // No explicit header: HTTP/1.1 defaults to keep-alive, HTTP/1.0 to close
    return version == "HTTP/1.1";
}

void HttpRequest::reset()
{
    method = HttpMethod::UNKNOWN;
    path.clear();
    version.clear();
    headers = HttpHeaders(); // Resets header map
    body.clear();
}