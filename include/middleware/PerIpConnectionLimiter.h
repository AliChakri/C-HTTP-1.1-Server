#pragma once

#include <string>
#include <unordered_map>
#include <mutex>

class PerIpConnectionLimiter
{
private:
    int max_per_ip_;
    std::mutex mtx_;
    std::unordered_map<std::string, int> counts_;    

public:
    explicit PerIpConnectionLimiter(int max_per_ip) : max_per_ip_(max_per_ip) {}

    // Return true and increments count if under the limit, false if already at cap
    bool try_acquire(const std::string& ip);

    void release(const std::string& ip);
};
