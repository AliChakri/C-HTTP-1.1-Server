#include "middleware/PerIpConnectionLimiter.h"

bool PerIpConnectionLimiter::try_acquire(const std::string& ip)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto& count = counts_[ip]; // Createdd with 0 if new
    if (count >= max_per_ip_) return false;
    ++count;
    return true;
}

void PerIpConnectionLimiter::release(const std::string& ip)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = counts_.find(ip);
    if (it == counts_.end()) return;
    if ( --(it->second) <= 0 ) counts_.erase(it); // cleanup, no leftover 0-entries
}