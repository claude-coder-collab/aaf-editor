#include "worker.hpp"

namespace aafedit
{

Worker::Worker() :
    thread_([this](const std::stop_token& stop) -> void { run(stop); })
{
}

Worker::~Worker()
{
    thread_.request_stop();
    ready_.notify_all();
}

void Worker::post(std::function<void()> task)
{
    {
        const std::scoped_lock lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    ready_.notify_one();
}

void Worker::run(const std::stop_token& stop)
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            if (!ready_.wait(lock, stop, [this] -> bool { return !tasks_.empty(); }))
            {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

}
