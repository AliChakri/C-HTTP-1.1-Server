#pragma once

#include "TaskQueue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>

class ThreadPool
{
private:
    static constexpr std::size_t WORKER_THREADS{64};

    std::array<std::thread, WORKER_THREADS> workers;

    TaskQueue *task_queue;

    std::atomic<bool> running{true};

    // Function executed by every worker thread.
    void worker();

public:
    ThreadPool(TaskQueue* taskQueue);
    ~ThreadPool();

    bool submit(std::function<void()> task);
};