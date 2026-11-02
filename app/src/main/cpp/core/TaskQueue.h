// Paint Engine - thread-safe task queue used to marshal work onto the render
// thread. Every GPU call in the engine happens on exactly one thread; other
// threads (UI / JNI) only post tasks here.
//
// Guarantees
//  * Tasks run in FIFO order on the consumer thread.
//  * An exception thrown by one task never prevents the following tasks of the
//    same batch from running (it is reported to the exception handler).
//  * After close(), post() refuses new tasks; the refused task object is
//    destroyed on the caller's thread. Pending tasks are destroyed (not run)
//    by clear().
//
// There is intentionally no "post and wait with timeout" helper: a caller
// that must know a task completed has to use its own completion signal, so a
// timeout can never be mistaken for completion.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace pe {

class TaskQueue {
public:
    using Task = std::function<void()>;
    using ExceptionHandler = std::function<void(const char* what)>;

    // Returns false (and drops the task) if the queue was closed.
    bool post(Task task);

    // Consumer side. Waits until a task is available (if `block`) or the queue
    // was woken, then runs every pending task. Returns tasks executed.
    size_t waitAndRun(bool block);

    // Wakes a blocked waitAndRun() without posting work.
    void wake();

    // Stops accepting tasks. Idempotent.
    void close();
    bool closed() const;

    // Drops pending tasks without running them.
    void clear();

    // Called on the consumer thread when a task throws.
    void setExceptionHandler(ExceptionHandler handler);

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    bool woken_ = false;
    bool closed_ = false;
    ExceptionHandler onException_;
};

}  // namespace pe
