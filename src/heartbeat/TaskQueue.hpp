#pragma once
// task_queue.hpp — Thread-safe task queue with dedicated worker thread (Layer 5)
//
// Provides a simple task queue consumed by a single worker jthread:
//   1. enqueue() — push a callable task (with optional priority)
//   2. Worker thread dequeues and executes tasks sequentially
//   3. Exceptions in tasks are caught and logged (worker never crashes)
//   4. Destructor signals stop and joins the worker thread
//
// Design rationale:
//   - Single worker thread prevents concurrent AI API calls
//   - Tasks are std::function<void()> — simple and composable
//   - Priority queue ensures urgent tasks execute before normal ones
//   - The heartbeat scheduler enqueues AI pipeline tasks here
//
// Thread safety:
//   - enqueue() is thread-safe (protected by mutex + condition_variable)
//   - Worker thread executes tasks sequentially (no concurrent execution)
//   - pendingCount() is thread-safe (mutex-protected read)

#include "heartbeat/heartbeat_events.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace pulse::heartbeat
{

// ---------------------------------------------------------------------------
// TaskQueue — thread-safe task queue with a single worker jthread
// ---------------------------------------------------------------------------
class TaskQueue
{
  public:
    /// Callable task type — any void() function or lambda.
    using TaskFn = std::function<void()>;

    /// Construct and spawn the worker thread.
    TaskQueue();

    /// Signal stop and join the worker thread.
    /// Any pending tasks are discarded.
    ~TaskQueue();

    /// Explicitly stop the worker thread before destructor runs.
    ///
    /// Signals the worker to stop, wakes it from the condition variable,
    /// and joins the thread.  Safe to call multiple times.  After stop(),
    /// the destructor join is a no-op (already joined).
    void stop();

    // Non-copyable, non-movable (owns a thread).
    TaskQueue(const TaskQueue &) = delete;
    TaskQueue &operator=(const TaskQueue &) = delete;
    TaskQueue(TaskQueue &&) = delete;
    TaskQueue &operator=(TaskQueue &&) = delete;

    /// Enqueue a task for the worker thread to execute.
    ///
    /// Parameters:
    ///   1. fn       — the callable to execute on the worker thread
    ///   2. priority — Normal (default) or High (inserted at front)
    void enqueue(TaskFn fn, TaskPriority priority = TaskPriority::Normal);

    /// Number of pending tasks in the queue.
    [[nodiscard]] std::size_t pendingCount() const;

    /// Whether the worker thread is running.
    [[nodiscard]] bool running() const;

  private:
    /// Internal task wrapper with priority.
    struct Task
    {
        TaskFn fn;
        TaskPriority priority;
    };

    /// Worker thread main loop — dequeues and executes tasks.
    ///
    /// Loop behavior:
    ///   1. Wait on condition_variable for a task or stop signal
    ///   2. Dequeue the front task (high priority first)
    ///   3. Execute the task inside a try/catch (log exceptions)
    ///   4. Check stop_token for cooperative cancellation
    ///
    /// Parameters:
    ///   1. stoken — stop_token for cooperative cancellation
    void workerLoop(std::stop_token stoken);

    std::jthread m_worker;                        ///< Dedicated worker thread.
    std::deque<Task> m_queue;                     ///< Pending tasks.
    mutable std::mutex m_mutex;                   ///< Protects m_queue.
    std::condition_variable m_cv;                 ///< Wakes worker on enqueue.
    std::atomic<bool> m_running{ false };         ///< Worker running state.
};

} // namespace pulse::heartbeat
