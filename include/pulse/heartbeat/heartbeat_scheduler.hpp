#pragma once
// heartbeat_scheduler.hpp — 5-minute AI analysis clock (Layer 5 Heartbeat)
//
// Drives the AI analysis pipeline on a fixed interval:
//   1. Uses asio::steady_timer for drift-free periodic ticks
//   2. On each tick, enqueues an AI pipeline task to the TaskQueue
//   3. The TaskQueue worker thread executes the pipeline (blocking I/O)
//   4. Timer is re-armed after task enqueue (not after task completion)
//
// Threading model:
//   - io_context thread: lightweight, only fires timer → enqueue → re-arm
//   - TaskQueue worker thread: blocks on HTTP I/O (AI pipeline execution)
//   - Main thread: creates, starts, and stops the scheduler
//
// Lifecycle:
//   1. Construct with config + pipeline reference + strategy params
//   2. start() — spawn io_context thread, arm first timer
//   3. Timer fires every heartbeatIntervalSec seconds
//   4. stop() — cancel timer, join io_context thread, TaskQueue drains
//
// Thread safety:
//   - start()/stop() must be called from the main thread (not concurrent)
//   - Timer callback executes on io_context thread (enqueues to TaskQueue)
//   - Pipeline execution happens on TaskQueue worker thread (isolated)

#include "pulse/ai/ai_pipeline.hpp"
#include "pulse/core/config.hpp"
#include "pulse/heartbeat/task_queue.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

namespace pulse::heartbeat
{

// ---------------------------------------------------------------------------
// HeartbeatScheduler — periodic AI analysis trigger
// ---------------------------------------------------------------------------
class HeartbeatScheduler
{
  public:
    /// Construct the scheduler.
    ///
    /// Parameters:
    ///   1. config  — AiConfig with heartbeat interval and retry settings
    ///   2. pipeline — reference to the AiPipeline to execute on each beat
    ///   3. params  — reference to the StrategyParams to update
    HeartbeatScheduler(const AiConfig &config,
                       ai::AiPipeline &pipeline,
                       strategy::StrategyParams &params);

    /// Destructor — calls stop() if still running.
    ~HeartbeatScheduler();

    // Non-copyable, non-movable (owns threads).
    HeartbeatScheduler(const HeartbeatScheduler &) = delete;
    HeartbeatScheduler &operator=(const HeartbeatScheduler &) = delete;
    HeartbeatScheduler(HeartbeatScheduler &&) = delete;
    HeartbeatScheduler &operator=(HeartbeatScheduler &&) = delete;

    /// Start the heartbeat loop — spawns io_context thread and arms timer.
    void start();

    /// Stop the heartbeat loop — cancels timer, joins threads.
    void stop();

    /// Manually trigger one AI analysis cycle (enqueues immediately).
    /// Useful for testing or on-demand analysis.
    void trigger_now();

    /// Number of beats that have fired since start.
    [[nodiscard]] std::uint64_t beat_count() const;

    /// Whether the scheduler is currently running.
    [[nodiscard]] bool running() const;

  private:
    /// Arm (or re-arm) the steady_timer for the next beat.
    void schedule_next();

    /// Timer callback — enqueues pipeline task and re-arms.
    ///
    /// Parameters:
    ///   1. ec — asio error code (non-zero if timer was cancelled)
    void on_timer(const asio::error_code &ec);

    /// Execute the AI pipeline on the TaskQueue worker thread.
    void run_pipeline();

    AiConfig config_;                            ///< Heartbeat configuration.
    ai::AiPipeline &pipeline_;                   ///< AI pipeline reference.
    strategy::StrategyParams &params_;           ///< Strategy params reference.

    asio::io_context io_ctx_;                    ///< ASIO I/O context for timer.
    asio::steady_timer timer_;                   ///< Periodic timer.
    std::jthread io_thread_;                     ///< Runs io_context::run().
    TaskQueue task_queue_;                       ///< Worker thread for pipeline.

    std::atomic<std::uint64_t> beat_count_{ 0 }; ///< Monotonic beat counter.
    std::atomic<bool> running_{ false };         ///< Scheduler running state.
};

} // namespace pulse::heartbeat
