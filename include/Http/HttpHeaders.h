#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <cstring>
#include <cctype>
#include <optional>
#include <cstdint>

class HttpHeaders
{
private:
    static std::string toLower(std::string str);
    std::unordered_map<std::string, std::string> m_headers;

public:
    HttpHeaders() = default;
    
    void set(std::string key, std::string value);
    std::optional<std::string> get(const std::string& key) const;
    void del(std::string key);
    bool has(const std::string& key) const;

    std::size_t getContentLength() const;

    const std::unordered_map<std::string, std::string>& getAll() const { return m_headers; }
};