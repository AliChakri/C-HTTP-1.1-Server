
#include "concurrency/TaskQueue.h"

bool TaskQueue::submit(std::function<void()> task)
{
    std::unique_lock<std::mutex> lock(queue_mtx);

    // Wait until there is space.
    not_full.wait(lock, [this] {
        return buffer_len < QUEUE_CAPACITY || shutdown;
    });

    // Don't accept new work after shutdown.
    if (shutdown) return false;

    // Put task at TAIL.
    tasks[tail] = std::move(task);
    // TAIL moves forward.
    tail = (tail + 1) % QUEUE_CAPACITY;
    // One more task exists.
    ++buffer_len;

    lock.unlock();

    // Wake one worker.
    not_empty.notify_one();
    return true;
}

std::function<void()> TaskQueue::remove()
{
    std::unique_lock<std::mutex> lock(queue_mtx);

    // Wait until there is at least one task.
    not_empty.wait(lock, [this] {
        return buffer_len > 0 || shutdown;
    });

    // If shutting down and there are no tasks left,
    // tell the worker that there is nothing more to do.
    if (buffer_len == 0 && shutdown)
    {
        return {};
    }

    // Take task from HEAD.
    std::function<void()> task = std::move(tasks[head]);

    // HEAD moves forward.
    head = (head + 1) % QUEUE_CAPACITY;

    // One less task exists.
    --buffer_len;

    lock.unlock();

    // Wake one producer.
    not_full.notify_one();

    return task;
}

void TaskQueue::stop() {
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        shutdown = true;
    }
    not_empty.notify_all();
    not_full.notify_all();
}
