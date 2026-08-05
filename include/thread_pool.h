#ifndef FORGELSM_THREAD_POOL_H
#define FORGELSM_THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <stop_token>

namespace forgelsm {

// A lightweight, C++20 thread pool using std::jthread.
// Dispatches async tasks, used primarily for background flushes and compactions.
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    // Disable copy/move
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a task to be executed by a worker thread
    void enqueue(std::function<void()> task);

    // Stop all background threads immediately
    void stop();

private:
    void worker_loop(std::stop_token stoken);

    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable_any cv_; // Used with jthread's stop_token
};

} // namespace forgelsm

#endif // FORGELSM_THREAD_POOL_H
