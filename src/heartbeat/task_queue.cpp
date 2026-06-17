// task_queue.cpp — TaskQueue implementation (Layer 5 Heartbeat)
//
// Worker thread lifecycle:
//   1. Constructor spawns worker jthread via worker_loop()
//   2. Worker blocks on condition_variable until a task arrives or stop is requested
//   3. Destructor signals stop, wakes the worker, and joins the thread

#include "pulse/heartbeat/task_queue.hpp"

#include "pulse/logging/logger.hpp"

namespace pulse::heartbeat
{

// ---------------------------------------------------------------------------
// Constructor — spawn worker thread
// ---------------------------------------------------------------------------
TaskQueue::TaskQueue()
    : worker_([this](std::stop_token stoken) { this->worker_loop(stoken); })
{
    running_.store(true, std::memory_order_release);
    PULSE_LOG_INFO("heartbeat", "TaskQueue worker thread started");
}

// ---------------------------------------------------------------------------
// Destructor — signal stop and join
// ---------------------------------------------------------------------------
TaskQueue::~TaskQueue()
{
    // 1. Signal the worker to stop
    worker_.request_stop();

    // 2. Wake the worker if it is blocked on the condition variable
    cv_.notify_one();

    // 3. worker_ is a jthread — destructor auto-joins
    running_.store(false, std::memory_order_release);
    PULSE_LOG_INFO("heartbeat", "TaskQueue worker thread stopped");
}

// ---------------------------------------------------------------------------
// enqueue — push a task onto the queue
//
// High-priority tasks are inserted at the front of the deque;
// normal-priority tasks are appended at the back.
// ---------------------------------------------------------------------------
void TaskQueue::enqueue(TaskFn fn, TaskPriority priority)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Task task{ std::move(fn), priority };
        if (TaskPriority::High == priority)
        {
            queue_.push_front(std::move(task));
        }
        else
        {
            queue_.push_back(std::move(task));
        }
    }
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// pending_count — thread-safe read of queue size
// ---------------------------------------------------------------------------
std::size_t TaskQueue::pending_count() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(mutex_));
    return queue_.size();
}

// ---------------------------------------------------------------------------
// running — whether the worker thread is active
// ---------------------------------------------------------------------------
bool TaskQueue::running() const
{
    return running_.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// worker_loop — main loop for the dedicated worker thread
//
// Execution flow:
//   1. Acquire mutex and wait on condition_variable
//   2. On wake: check stop_token → if stop requested, exit loop
//   3. Dequeue front task (if queue is non-empty)
//   4. Release mutex before executing the task
//   5. Execute task inside try/catch — log any exceptions
//   6. Loop back to step 1
// ---------------------------------------------------------------------------
void TaskQueue::worker_loop(std::stop_token stoken)
{
    while (!stoken.stop_requested())
    {
        Task task;

        // 1. Wait for a task or stop signal
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&stoken, this]
            {
                return stoken.stop_requested() || !queue_.empty();
            });

            // 2. Check stop request after waking
            if (stoken.stop_requested())
            {
                break;
            }

            // 3. Dequeue the front task
            if (queue_.empty())
            {
                continue; // Spurious wakeup
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        // 4. Mutex released — execute the task without holding the lock

        // 5. Execute with exception safety — worker must never crash
        if (task.fn)
        {
            try
            {
                task.fn();
            }
            catch (const std::exception &e)
            {
                PULSE_LOG_ERROR("heartbeat", "TaskQueue: task threw exception: {}", e.what());
            }
            catch (...)
            {
                PULSE_LOG_ERROR("heartbeat", "TaskQueue: task threw unknown exception");
            }
        }
    }
}

} // namespace pulse::heartbeat
