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
    : m_maxRate{ max_orders_per_sec }
    , m_burstCapacity{ (0 == burst_capacity) ? max_orders_per_sec * 2 : burst_capacity }
    , m_tokens{ static_cast<double>(m_burstCapacity) }
    , m_lastRefillNs{ nowNs() }
{
    // Initialize bucket to full burst capacity.
    if (0 == m_burstCapacity)
    {
        m_burstCapacity = m_maxRate * 2;
        m_tokens.store(static_cast<double>(m_burstCapacity), std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool OrderRateLimiter::tryAcquire()
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
        double current = m_tokens.load(std::memory_order_acquire);

        if (current < 1.0)
        {
            PULSE_LOG_WARN("risk",
                "Rate limiter: bucket empty ({:.2f} tokens), rejecting order", current);
            return false;
        }

        if (m_tokens.compare_exchange_weak(current, current - 1.0,
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

double OrderRateLimiter::availableTokens() const
{
    return m_tokens.load(std::memory_order_acquire);
}

bool OrderRateLimiter::isExhausted() const
{
    return m_tokens.load(std::memory_order_acquire) < 1.0;
}

void OrderRateLimiter::reset()
{
    m_tokens.store(static_cast<double>(m_burstCapacity), std::memory_order_release);
    m_lastRefillNs.store(nowNs(), std::memory_order_release);
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

    const std::int64_t current_ns = nowNs();
    const std::int64_t last_ns = m_lastRefillNs.load(std::memory_order_acquire);
    const std::int64_t elapsed_ns = current_ns - last_ns;

    if (elapsed_ns <= 0)
    {
        return; // No time elapsed or clock went backwards.
    }

    const double new_tokens = static_cast<double>(elapsed_ns) * m_maxRate / 1.0e9;

    if (new_tokens < 0.001)
    {
        return; // Too small to bother updating.
    }

    // Update tokens: current + new_tokens, capped at burst_capacity.
    double current = m_tokens.load(std::memory_order_acquire);
    double updated = current + new_tokens;

    if (updated > static_cast<double>(m_burstCapacity))
    {
        updated = static_cast<double>(m_burstCapacity);
    }

    // CAS to update tokens; if it fails, another thread already refilled.
    if (m_tokens.compare_exchange_weak(current, updated,
        std::memory_order_acq_rel, std::memory_order_acquire))
    {
        // Successfully refilled — update the timestamp.
        std::int64_t expected_ns = last_ns;
        m_lastRefillNs.compare_exchange_strong(expected_ns, current_ns,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
}

std::int64_t OrderRateLimiter::nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace pulse::risk
