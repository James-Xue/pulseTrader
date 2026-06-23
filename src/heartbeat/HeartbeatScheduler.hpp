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

#include "ai/AiPipeline.hpp"
#include "core/config.hpp"
#include "heartbeat/TaskQueue.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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
    ///   1. config     — AiConfig with heartbeat interval and retry settings
    ///   2. pipeline   — reference to the AiPipeline to execute on each beat
    ///   3. allParams — mutable pointers to each strategy's StrategyParams;
    ///                   the AI pipeline writes deltas to all of them
    HeartbeatScheduler(const AiConfig &config,
                       ai::AiPipeline &pipeline,
                       std::vector<strategy::StrategyParams *> allParams);

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
    void triggerNow();

    /// Number of beats that have fired since start.
    [[nodiscard]] std::uint64_t beatCount() const;

    /// Whether the scheduler is currently running.
    [[nodiscard]] bool running() const;

  private:
    /// Arm (or re-arm) the steady_timer for the next beat.
    void scheduleNext();

    /// Timer callback — enqueues pipeline task and re-arms.
    ///
    /// Parameters:
    ///   1. ec — asio error code (non-zero if timer was cancelled)
    void onTimer(const asio::error_code &ec);

    /// Execute the AI pipeline on the TaskQueue worker thread.
    void runPipeline();

    AiConfig m_config;                            ///< Heartbeat configuration.
    ai::AiPipeline &m_pipeline;                   ///< AI pipeline reference.
    std::vector<strategy::StrategyParams *> m_allParams; ///< Per-strategy params.

    asio::io_context m_ioCtx;                    ///< ASIO I/O context for timer.
    asio::steady_timer m_timer;                   ///< Periodic timer.
    std::jthread m_ioThread;                     ///< Runs io_context::run().
    TaskQueue m_taskQueue;                       ///< Worker thread for pipeline.

    std::atomic<std::uint64_t> m_beatCount{ 0 }; ///< Monotonic beat counter.
    std::atomic<bool> m_running{ false };         ///< Scheduler running state.
};

} // namespace pulse::heartbeat
