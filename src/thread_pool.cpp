#include "thread_pool.h"

namespace forgelsm {

ThreadPool::ThreadPool(size_t num_threads) {
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this](std::stop_token stoken) {
            this->worker_loop(std::move(stoken));
        });
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::stop() {
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    cv_.notify_all();
    workers_.clear();
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::worker_loop(std::stop_token stoken) {
    while (!stoken.stop_requested()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            // Wait until a task is available or a stop is requested.
            // Using condition_variable_any seamlessly integrates with std::stop_token.
            cv_.wait(lock, stoken, [this]() {
                return !tasks_.empty();
            });

            if (stoken.stop_requested() && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // Execute task outside the lock.
        task();
    }
}

} // namespace forgelsm
