#pragma once
// order_rate_limiter.hpp — Token-bucket rate limiter (Layer 7 Risk Management)
//
// Implements a token-bucket algorithm to enforce Gate.io order submission
// rate limits and prevent API bans:
//   1. Tokens are refilled at a fixed rate (max_orders_per_sec)
//   2. Each order submission consumes one token
//   3. If the bucket is empty, the order is rejected
//   4. Burst capacity allows short bursts above the sustained rate
//
// Thread safety:
//   - std::atomic for token count and last-refill timestamp
//   - Lock-free on the hot path (CAS loop for acquire and refill)

#include <atomic>
#include <cstdint>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// OrderRateLimiter — token-bucket rate limiter for order submissions
// ---------------------------------------------------------------------------
class OrderRateLimiter
{
  public:
    /// Construct with capacity and optional burst multiplier.
    ///
    /// Parameters:
    ///   1. max_orders_per_sec — sustained rate (tokens refilled per second)
    ///   2. burst_capacity     — max bucket size (default: 2x rate)
    explicit OrderRateLimiter(std::uint32_t max_orders_per_sec, std::uint32_t burst_capacity = 0);

    /// Try to consume one token.
    ///
    /// Algorithm:
    ///   1. Call refill() to add tokens based on elapsed time
    ///   2. Load m_tokens atomically
    ///   3. If tokens >= 1.0: CAS(tokens - 1.0); return true on success
    ///   4. If tokens < 1.0: return false (rate limited)
    ///
    /// Returns true if allowed, false if rate-limited.
    [[nodiscard]] bool tryAcquire();

    /// Returns the current number of available tokens (approximate, lock-free read).
    [[nodiscard]] double availableTokens() const;

    /// Returns true if the bucket is empty (no tokens available).
    [[nodiscard]] bool isExhausted() const;

    /// Reset the bucket to full capacity (for testing or manual override).
    void reset();

  private:
    std::uint32_t m_maxRate;        ///< Tokens per second (refill rate).
    std::uint32_t m_burstCapacity;  ///< Maximum tokens in bucket.
    std::atomic<double> m_tokens;    ///< Current available tokens.
    std::atomic<std::int64_t> m_lastRefillNs; ///< Last refill timestamp (nanoseconds since epoch).

    /// Add tokens based on elapsed time since last refill.
    /// Uses CAS to avoid double-refill under contention.
    void refill();

    /// Get current time in nanoseconds since epoch.
    [[nodiscard]] static std::int64_t nowNs();
};

} // namespace pulse::risk
