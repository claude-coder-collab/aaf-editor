#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace aafedit
{

/// Runs posted tasks one at a time, in order, on a background thread.
class Worker
{
public:
    Worker();
    ~Worker();
    Worker(const Worker&) = delete;
    Worker(Worker&&) = delete;
    auto operator=(const Worker&) -> Worker& = delete;
    auto operator=(Worker&&) -> Worker& = delete;

    void post(std::function<void()> task);

private:
    void run(const std::stop_token& stop);

    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<std::function<void()>> tasks_;
    std::jthread thread_;
};

}
