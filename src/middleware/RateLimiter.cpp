#include "middleware/RateLimiter.h"

RateLimiter::RateLimiter(double requests_per_second, double burst_capacity)
            : rate_(requests_per_second), capacity_(burst_capacity)
{
}

size_t RateLimiter::shard_for(const std::string& ip) const
{
    return std::hash<std::string>{}(ip) % NUM_SHARDS;
}

bool RateLimiter::allow(const std::string& ip)
{
    size_t shard_idx = shard_for(ip);
    Bucket* bucket = nullptr;

    // --- Step 1: find or create the bucket for this IP ---
    // Lock only the shard's own mutex (not a global one), and only for the
    // duration of the lookup/insert — NOT for the token math below, so we
    // don't hold this lock any longer than necessary.
    {
        std::lock_guard<std::mutex> shard_lock(shard_mutexes_[shard_idx]);
        auto& map = shards_[shard_idx];
        auto it = map.find(ip);
        if (it == map.end())
        {
            auto new_bucket = std::make_unique<Bucket>();
            new_bucket->tokens = capacity_; // starts full - first burst is allowed
            new_bucket->last_refill = std::chrono::steady_clock::now();
            bucket = new_bucket.get(); // return raw pointer
            map.emplace(ip, std::move(new_bucket));
        }
        else
        {
            bucket = it->second.get(); // Returns raw pointer
        }
    }
    // Note: `bucket` stays valid even after we unlock the shard mutex above,
    // because it's a heap object owned by unique_ptr inside the map — a
    // rehash may move map internals around, but never relocates the Bucket
    // object itself.


    // --- Step 2: refill + consume, protected by this bucket's OWN mutex ---
    // This is the part that would otherwise serialize all requests from the
    // same IP; using a per-bucket mutex means different IPs never block
    // each other here either.
    std::lock_guard<std::mutex> bucket_lock(bucket->mtx);

    auto now = std::chrono::steady_clock::now();
    auto time_elapsed = std::chrono::duration<double>(now - bucket->last_refill).count();
    bucket->last_refill = now;

    bucket->tokens = std::min(capacity_, bucket->tokens + time_elapsed * rate_);

    if (bucket->tokens >= 1.0)
    {
        bucket->tokens -= 1.0;
        return true;
    }

    return false; // Rate Limitted
}

void RateLimiter::sweep_stale(std::chrono::seconds max_idle)
{
    auto now = std::chrono::steady_clock::now();

    for (size_t i = 0; i < NUM_SHARDS; ++i)
    {
        std::lock_guard<std::mutex> shard_lock(shard_mutexes_[i]);
        auto& map = shards_[i];

        for (auto it = map.begin(); it != map.end(); )
        {
            std::lock_guard<std::mutex> bucket_lock(it->second->mtx);
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second->last_refill
            );

            if (idle > max_idle)
            {
                // No increment since erase with itertators return the next iterator
                it = map.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}