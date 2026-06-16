#pragma once
// kline_buffer.hpp — Thread-safe ring buffer for K-line candles (Layer 3 Market Data)
//
// Stores the N most recent K-line candles per symbol/interval. Uses a fixed-size
// ring buffer to avoid dynamic allocation on the hot path. Readers (strategy threads)
// obtain a consistent snapshot via a seqlock pattern — no locks held during the copy.
//
// Thread safety:
//   - push() is called from the WebSocket I/O thread (exclusive write)
//   - snapshot() and latest() are called from strategy threads (lock-free read)
//   - Seqlock ensures readers see a consistent snapshot or retry on conflict

#include "pulse/core/types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulse::market
{

// ---------------------------------------------------------------------------
// Kline — one candlestick bar
//
// POD struct for trivial copyability (required for atomic operations in seqlock).
// ---------------------------------------------------------------------------
struct Kline
{
    std::int64_t open_time;  ///< Candle open time (Unix ms).
    std::int64_t close_time; ///< Candle close time (Unix ms).
    Price open;              ///< Opening price.
    Price high;              ///< Highest price during the interval.
    Price low;               ///< Lowest price during the interval.
    Price close;             ///< Closing price (or last price if still open).
    Quantity volume;         ///< Trading volume in base currency.
    bool closed;             ///< True if the candle is finalized (interval elapsed).

    /// Default constructor — zero-initializes all fields.
    Kline()
        : open_time{ 0 }
        , close_time{ 0 }
        , open{ 0.0 }
        , high{ 0.0 }
        , low{ 0.0 }
        , close{ 0.0 }
        , volume{ 0.0 }
        , closed{ false }
    {
    }
};

// Verify Kline is trivially copyable (required for seqlock atomic copy).
static_assert(std::is_trivially_copyable_v<Kline>, "Kline must be trivially copyable");

// ---------------------------------------------------------------------------
// KlineBuffer — fixed-size ring buffer with seqlock reads
//
// Usage:
//   KlineBuffer buffer(500);  // store last 500 candles
//   buffer.push(kline);       // from WS thread
//   auto snap = buffer.snapshot(100);  // from strategy thread (last 100 candles)
//   auto latest = buffer.latest();     // most recent candle
// ---------------------------------------------------------------------------
class KlineBuffer
{
  public:
    /// Construct a ring buffer with the given capacity.
    ///
    /// Parameters:
    ///   1. capacity — maximum number of candles to store (default 500)
    explicit KlineBuffer(std::size_t capacity = 500);

    /// Push a new candle into the buffer.
    ///
    /// If the buffer is full, the oldest candle is overwritten.
    /// This method increments the seqlock counter before and after the write
    /// to signal readers that a write is in progress.
    ///
    /// Parameters:
    ///   1. kline — the candle to store (copied atomically)
    void push(const Kline &kline);

    /// Retrieve a snapshot of the last N candles (lock-free seqlock read).
    ///
    /// Returns a vector of N candles in chronological order (oldest first).
    /// If fewer than N candles are available, returns all available candles.
    ///
    /// The seqlock pattern ensures the snapshot is consistent:
    ///   1. Read the seqlock counter (must be even = no write in progress)
    ///   2. Copy the requested candles
    ///   3. Read the seqlock counter again (must match step 1 and be even)
    ///   4. If counters differ or odd, retry (a write occurred during the copy)
    ///
    /// Parameters:
    ///   1. n — number of candles to retrieve (default: all available)
    [[nodiscard]] std::vector<Kline> snapshot(std::size_t n = 0) const;

    /// Retrieve the most recent candle (lock-free seqlock read).
    ///
    /// Returns std::nullopt if the buffer is empty.
    [[nodiscard]] std::optional<Kline> latest() const;

    /// Returns the number of candles currently stored.
    [[nodiscard]] std::size_t size() const;

    /// Returns the buffer capacity.
    [[nodiscard]] std::size_t capacity() const;

  private:
    std::size_t capacity_;
    std::vector<Kline> buffer_; ///< Ring buffer storage.
    std::atomic<std::size_t> head_{ 0 }; ///< Next write position.
    std::atomic<std::size_t> count_{ 0 }; ///< Number of valid candles.
    std::atomic<std::uint64_t> seqlock_{ 0 }; ///< Even = no write; odd = write in progress.

    /// Compute the buffer index for a given logical position.
    ///
    /// Logical position 0 is the oldest candle; position (count-1) is the newest.
    /// This method maps logical positions to physical ring buffer indices.
    [[nodiscard]] std::size_t logical_to_physical(std::size_t logical_pos) const;
};

} // namespace pulse::market
