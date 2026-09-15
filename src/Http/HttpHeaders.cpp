#include "Http/HttpHeaders.h"

#include <algorithm>

std::string HttpHeaders::toLower(std::string str)
{
    std::transform(str.begin(), str.end(), str.begin(), 
                    [](unsigned char c) { return std::tolower(c); });
    
    return str;
}

void HttpHeaders::set(std::string key, std::string value)
{
    m_headers[toLower(std::move(key))] = std::move(value);
}

std::optional<std::string> HttpHeaders::get(const std::string& key) const
{
    auto it = m_headers.find(toLower(key));
    if (it != m_headers.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void HttpHeaders::del(std::string key)
{
    auto it = m_headers.find(toLower(key));
    if (it != m_headers.end())
    {
        m_headers.erase(key);
        return;
    }
    return;
}

bool HttpHeaders::has(const std::string& key) const
{
    return m_headers.find(toLower(key)) != m_headers.end();
}

std::size_t HttpHeaders::getContentLength() const
{
    auto val = get("content-length");
    if (!val) return 0;
    try
    {
        return std::stoull(*val);
    }
    catch(...)
    {
        return 0;
    }
    
}