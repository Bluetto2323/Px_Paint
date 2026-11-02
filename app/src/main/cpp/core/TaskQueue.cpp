#include "core/TaskQueue.h"

#include <exception>

#include "core/Log.h"

namespace pe {

bool TaskQueue::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;  // `task` destroyed by the caller's frame
        tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

size_t TaskQueue::waitAndRun(bool block) {
    std::deque<Task> batch;
    ExceptionHandler handler;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (block) cv_.wait(lock, [this] { return !tasks_.empty() || woken_; });
        woken_ = false;
        batch.swap(tasks_);
        handler = onException_;
    }
    for (auto& task : batch) {
        try {
            task();
        } catch (const std::exception& e) {
            PE_LOGE("TaskQueue", "task threw: %s", e.what());
            if (handler) handler(e.what());
        } catch (...) {
            PE_LOGE("TaskQueue", "task threw a non-standard exception");
            if (handler) handler("unknown exception");
        }
    }
    return batch.size();
}

void TaskQueue::wake() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        woken_ = true;
    }
    cv_.notify_one();
}

void TaskQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}

bool TaskQueue::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

void TaskQueue::clear() {
    std::deque<Task> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dropped.swap(tasks_);
    }
    // Destroyed outside the lock: a dropped task may own a window whose
    // destructor must not run while holding the queue mutex.
}

void TaskQueue::setExceptionHandler(ExceptionHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    onException_ = std::move(handler);
}

}  // namespace pe
