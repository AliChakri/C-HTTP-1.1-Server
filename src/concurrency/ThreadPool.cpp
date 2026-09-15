#include "concurrency/ThreadPool.h"

ThreadPool::ThreadPool(TaskQueue *taskQueue) : task_queue(taskQueue) 
{
    for(size_t i = 0; i < WORKER_THREADS; i++) {
        workers[i] = std::thread(&ThreadPool::worker, this);
    }
}

ThreadPool::~ThreadPool()
{
    // Tell the queue that no more work will be accepted.
    task_queue->stop();

    // Tell workers that the pool is shutting down.
    running.store(false);

    //  Gracefull Shutdown Wait for every worker.
    for (std::thread& worker : workers)
    {
        if (worker.joinable())  worker.join();
    }
}

void ThreadPool::worker() 
{
    while (true)
    {
        auto task = task_queue->remove();

        // Queue has shut down and there are no tasks.
        if (!task) {
            break;
        }

        task();
    }
}

bool ThreadPool::submit(std::function<void()> task) {
    return task_queue->submit(std::move(task));
}
