#pragma once

#include "Utils/Logger.h"

#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <array>
#include <chrono>

class RateLimiter
{
private:
    struct Bucket
    {
        double tokens;
        std::chrono::steady_clock::time_point last_refill;
        std::mutex mtx; // Protect this Bucket's own fields only
    };

    static constexpr size_t NUM_SHARDS = 16;

    double rate_;      // tokens added per second
    double capacity_;  // max tokens (burst ceiling)

    std::array<std::mutex, NUM_SHARDS> shard_mutexes_;
    std::array<std::unordered_map<std::string, std::unique_ptr<Bucket>>, NUM_SHARDS> shards_;

    // fast simple distribution for shard selection
    size_t shard_for(const std::string& ip) const;
    
public:
    // Requests per second: steady state allowed rate
    // Busrt Capacity: Max tokens a bucket can hold
    RateLimiter(double requests_per_second, double burst_capacity);

    // Returns true if this request is allowed (consumes one token), false if rate-limited.
    bool allow(const std::string& ip);

    // Call periodically (e.g. from your existing idle-reaper thread) to evict
    // buckets for IPs that haven't been seen in a while, so memory doesn't
    // grow unbounded from one-off/rotating IPs.
    void sweep_stale(std::chrono::seconds max_idle = std::chrono::seconds(300));
};
