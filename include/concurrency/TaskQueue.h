#pragma once

#include <iostream>
#include <array>
#include <string>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>

class TaskQueue
{
private:
    static constexpr std::size_t QUEUE_CAPACITY{1024};
    std::array<std::function<void()>, QUEUE_CAPACITY> tasks {};

    // Ring Buffer
    std::size_t head {0};
    std::size_t tail {0};
    std::size_t buffer_len { 0 };

    std::mutex queue_mtx;

    std::condition_variable not_full;
    std::condition_variable not_empty;

    bool shutdown{false};

public:
    TaskQueue() = default;
    ~TaskQueue() = default;

    bool submit(std::function<void()> task);
    std::function<void()> remove();
    void stop();
};