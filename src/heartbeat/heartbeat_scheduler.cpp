// heartbeat_scheduler.cpp — HeartbeatScheduler implementation (Layer 5)
//
// Timer lifecycle:
//   1. start() creates io_context thread and arms the first timer
//   2. Timer fires → onTimer() → enqueues pipeline task → scheduleNext()
//   3. stop() cancels timer (onTimer gets error::operation_aborted)
//   4. io_context thread exits when it has no more work

#include "heartbeat/heartbeat_scheduler.hpp"

#include "logging/logger.hpp"

#include <asio/post.hpp>

namespace pulse::heartbeat
{

// ---------------------------------------------------------------------------
// Constructor — initialize timer but do not start
// ---------------------------------------------------------------------------
HeartbeatScheduler::HeartbeatScheduler(const AiConfig &config,
                                       ai::AiPipeline &pipeline,
                                       std::vector<strategy::StrategyParams *> allParams)
    : m_config{ config }
    , m_pipeline{ pipeline }
    , m_allParams{ std::move(allParams) }
    , m_timer{ m_ioCtx }
{
    PULSE_LOG_INFO("heartbeat", "HeartbeatScheduler created (interval={}s, {} strategy params)",
                   m_config.heartbeatIntervalSec, m_allParams.size());
}

// ---------------------------------------------------------------------------
// Destructor — stop if still running
// ---------------------------------------------------------------------------
HeartbeatScheduler::~HeartbeatScheduler()
{
    if (m_running.load(std::memory_order_acquire))
    {
        stop();
    }
}

// ---------------------------------------------------------------------------
// start — spawn io_context thread and arm first timer
// ---------------------------------------------------------------------------
void HeartbeatScheduler::start()
{
    if (m_running.load(std::memory_order_acquire))
    {
        PULSE_LOG_WARN("heartbeat", "HeartbeatScheduler already running");
        return;
    }

    m_running.store(true, std::memory_order_release);
    m_beatCount.store(0, std::memory_order_release);

    // 1. Arm the first timer before starting the io_context thread
    scheduleNext();

    // 2. Spawn the io_context thread
    m_ioThread = std::jthread([this](std::stop_token stoken)
    {
        // Run io_context until stopped or no more work
        while (!stoken.stop_requested())
        {
            // run_for returns when all work is done or timeout
            // This allows checking stop_token periodically
            m_ioCtx.run_for(std::chrono::milliseconds(100));

            // If io_context has no more work and timer is not armed, exit
            if (m_ioCtx.stopped())
            {
                break;
            }
        }
    });

    PULSE_LOG_INFO("heartbeat", "HeartbeatScheduler started");
}

// ---------------------------------------------------------------------------
// stop — cancel timer, drain io_context, join threads
//
// Shutdown order:
//   1. Signal running = false
//   2. Post timer cancellation to io_context thread
//   3. Stop io_context (prevents new work from being queued)
//   4. Request stop on io_thread (jthread)
//   5. TaskQueue destructor joins its worker thread
// ---------------------------------------------------------------------------
void HeartbeatScheduler::stop()
{
    if (!m_running.load(std::memory_order_acquire))
    {
        return;
    }

    m_running.store(false, std::memory_order_release);

    // 1. Cancel the timer (onTimer will get operation_aborted)
    asio::error_code ec;
    m_timer.cancel(ec);

    // 2. Stop the io_context
    m_ioCtx.stop();

    // 3. Request stop on io_thread and let jthread join
    m_ioThread.request_stop();

    // 4. TaskQueue destructor (called when this object is destroyed)
    //    will join its worker thread

    PULSE_LOG_INFO("heartbeat", "HeartbeatScheduler stopped (beats={})",
                   m_beatCount.load(std::memory_order_acquire));
}

// ---------------------------------------------------------------------------
// triggerNow — manually enqueue a pipeline task
// ---------------------------------------------------------------------------
void HeartbeatScheduler::triggerNow()
{
    auto beat_num = m_beatCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    PULSE_LOG_INFO("heartbeat", "Manual trigger: beat #{}", beat_num);

    m_taskQueue.enqueue([this]() { this->runPipeline(); }, TaskPriority::High);
}

// ---------------------------------------------------------------------------
// beatCount — monotonic counter of beats fired
// ---------------------------------------------------------------------------
std::uint64_t HeartbeatScheduler::beatCount() const
{
    return m_beatCount.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// running — whether the scheduler is active
// ---------------------------------------------------------------------------
bool HeartbeatScheduler::running() const
{
    return m_running.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// scheduleNext — arm the steady_timer for the next beat
//
// Uses expires_after() (not expires_at()) for drift-free scheduling:
// the interval starts from when this function is called, not from
// when the previous timer was armed. This prevents accumulated drift.
// ---------------------------------------------------------------------------
void HeartbeatScheduler::scheduleNext()
{
    m_timer.expires_after(std::chrono::seconds(m_config.heartbeatIntervalSec));
    m_timer.async_wait([this](const asio::error_code &ec) { this->onTimer(ec); });
}

// ---------------------------------------------------------------------------
// onTimer — callback when the steady_timer fires
//
// Behavior:
//   1. If cancelled (operation_aborted) → do nothing (shutdown in progress)
//   2. Increment beat counter
//   3. Enqueue pipeline task to TaskQueue
//   4. Re-arm timer for next beat
// ---------------------------------------------------------------------------
void HeartbeatScheduler::onTimer(const asio::error_code &ec)
{
    // 1. Timer was cancelled — likely due to stop()
    if (ec)
    {
        PULSE_LOG_INFO("heartbeat", "Timer cancelled: {}", ec.message());
        return;
    }

    // 2. Check if we should still be running
    if (!m_running.load(std::memory_order_acquire))
    {
        return;
    }

    // 3. Increment beat counter
    auto beat_num = m_beatCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    PULSE_LOG_INFO("heartbeat", "Beat #{} — enqueueing AI pipeline", beat_num);

    // 4. Enqueue the pipeline task (executed on TaskQueue worker thread)
    m_taskQueue.enqueue([this]() { this->runPipeline(); }, TaskPriority::Normal);

    // 5. Re-arm the timer for the next beat
    scheduleNext();
}

// ---------------------------------------------------------------------------
// runPipeline — execute the AI pipeline (called on TaskQueue worker thread)
//
// This is the blocking function that performs the full AI cycle:
//   1. Poll social feeds (HTTP I/O)
//   2. Build prompt
//   3. Call LLM (HTTP I/O)
//   4. Parse and validate response
//   5. Apply parameter deltas atomically
//
// All errors are handled internally — this function never throws.
// ---------------------------------------------------------------------------
void HeartbeatScheduler::runPipeline()
{
    PULSE_LOG_INFO("heartbeat", "AI pipeline execution started");

    // Create a minimal market snapshot for now
    // In production, this would come from MarketFeed / TickerCache
    ai::MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";

    // Run the pipeline — errors are logged internally
    auto result = m_pipeline.run(snapshot, m_allParams);

    if (ok(result))
    {
        PULSE_LOG_INFO("heartbeat", "AI pipeline completed: sentiment={}, confidence={:.2f}",
                       toString(value(result).sentiment),
                       value(result).confidence);
    }
    else
    {
        PULSE_LOG_WARN("heartbeat", "AI pipeline failed: [{}] {}",
                       static_cast<std::uint32_t>(error(result).code),
                       error(result).message);
    }
}

} // namespace pulse::heartbeat
