// order_rate_limiter.cpp — Token-bucket rate limiter (Layer 7 Risk Management)

#include "risk/order_rate_limiter.hpp"

#include "logging/logger.hpp"

#include <chrono>

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OrderRateLimiter::OrderRateLimiter(std::uint32_t max_orders_per_sec, std::uint32_t burst_capacity)
    : max_rate_{ max_orders_per_sec }
    , burst_capacity_{ (0 == burst_capacity) ? max_orders_per_sec * 2 : burst_capacity }
    , tokens_{ static_cast<double>(burst_capacity_) }
    , last_refill_ns_{ now_ns() }
{
    // Initialize bucket to full burst capacity.
    if (0 == burst_capacity_)
    {
        burst_capacity_ = max_rate_ * 2;
        tokens_.store(static_cast<double>(burst_capacity_), std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool OrderRateLimiter::try_acquire()
{
    // Try to consume one token from the bucket:
    // 1. Refill tokens based on elapsed time
    // 2. Load current tokens
    // 3. If >= 1.0: CAS to consume one token
    // 4. On CAS failure: retry (another thread consumed concurrently)
    // 5. If < 1.0: return false (rate limited)

    refill();

    // CAS loop to atomically consume one token.
    constexpr int kMaxRetries = 10;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt)
    {
        double current = tokens_.load(std::memory_order_acquire);

        if (current < 1.0)
        {
            PULSE_LOG_WARN("risk",
                "Rate limiter: bucket empty ({:.2f} tokens), rejecting order", current);
            return false;
        }

        if (tokens_.compare_exchange_weak(current, current - 1.0,
            std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return true;
        }
        // CAS failed — another thread consumed; retry.
    }

    // Exhausted retries under extreme contention — reject.
    PULSE_LOG_WARN("risk", "Rate limiter: CAS contention exceeded max retries, rejecting");
    return false;
}

double OrderRateLimiter::available_tokens() const
{
    return tokens_.load(std::memory_order_acquire);
}

bool OrderRateLimiter::is_exhausted() const
{
    return tokens_.load(std::memory_order_acquire) < 1.0;
}

void OrderRateLimiter::reset()
{
    tokens_.store(static_cast<double>(burst_capacity_), std::memory_order_release);
    last_refill_ns_.store(now_ns(), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void OrderRateLimiter::refill()
{
    // Add tokens based on elapsed time since last refill:
    // 1. Load last refill timestamp
    // 2. Compute elapsed nanoseconds
    // 3. Compute new tokens: elapsed_ns * rate / 1e9
    // 4. CAS update tokens (capped at burst_capacity)
    // 5. CAS update last_refill_ns

    const std::int64_t current_ns = now_ns();
    const std::int64_t last_ns = last_refill_ns_.load(std::memory_order_acquire);
    const std::int64_t elapsed_ns = current_ns - last_ns;

    if (elapsed_ns <= 0)
    {
        return; // No time elapsed or clock went backwards.
    }

    const double new_tokens = static_cast<double>(elapsed_ns) * max_rate_ / 1.0e9;

    if (new_tokens < 0.001)
    {
        return; // Too small to bother updating.
    }

    // Update tokens: current + new_tokens, capped at burst_capacity.
    double current = tokens_.load(std::memory_order_acquire);
    double updated = current + new_tokens;

    if (updated > static_cast<double>(burst_capacity_))
    {
        updated = static_cast<double>(burst_capacity_);
    }

    // CAS to update tokens; if it fails, another thread already refilled.
    if (tokens_.compare_exchange_weak(current, updated,
        std::memory_order_acq_rel, std::memory_order_acquire))
    {
        // Successfully refilled — update the timestamp.
        std::int64_t expected_ns = last_ns;
        last_refill_ns_.compare_exchange_strong(expected_ns, current_ns,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
}

std::int64_t OrderRateLimiter::now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace pulse::risk
