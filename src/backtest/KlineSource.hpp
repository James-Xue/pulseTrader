#pragma once
// kline_source.hpp — Data source abstraction for historical candles (M29)
//
// The backtest engine can load candles from two places:
//   1. SqliteKlineReader — the engine's own kline_bars capture (local truth)
//   2. GateKlineFetcher  — the exchange REST API (fills historical gaps)
//
// KlineLoader orchestrates both behind this interface: SQLite first, API for
// the missing ranges, merge + dedup, optional cache write-back.

#include "backtest/backtest_types.hpp"
#include "core/PulseError.hpp"
#include "core/config.hpp"
#include "market/KlineBuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// IKlineSource — one historical candle provider
// ---------------------------------------------------------------------------
class IKlineSource
{
  public:
    virtual ~IKlineSource() = default;

    /// Fetch closed candles whose open_time falls in [from_ms, to_ms].
    ///
    /// Implementations return candles sorted ascending by open_time and
    /// deduplicated. May return fewer candles than the range covers (gaps are
    /// expected); it must not return candles outside the range.
    [[nodiscard]] virtual Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) = 0;

    /// Short human-readable source label (for logs / report header).
    [[nodiscard]] virtual std::string description() const = 0;

    /// Optional persistence hook: sources that can cache candles (e.g. the
    /// SQLite reader) write them back; the default is a no-op returning 0.
    /// The loader calls this with the merged candle set so API-fetched rows
    /// become local data for the next run.
    [[nodiscard]] virtual Result<std::size_t> writeBack(
        const std::string &symbol, MarketType market_type,
        const std::vector<market::Kline> &candles)
    {
        (void)symbol;
        (void)market_type;
        (void)candles;
        return std::size_t{ 0 };
    }
};

// ---------------------------------------------------------------------------
// findKlineGaps — pure gap detection over sorted candles
//
// Returns the [from_ms, to_ms] sub-ranges (candle open times, inclusive)
// inside the requested window that have NO candle in `rows`. Handles:
//   - empty rows        → the whole window is one gap
//   - gap at the head   → [from_ms, first.open_time - interval_ms]
//   - gap in the middle → [a.open_time + interval_ms, b.open_time - interval_ms]
//   - gap at the tail   → [last.open_time + interval_ms, to_ms]
//
// `rows` must be sorted ascending by open_time (loader guarantees this).
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> findKlineGaps(
    const std::vector<market::Kline> &rows,
    std::int64_t from_ms, std::int64_t to_ms, std::int64_t interval_ms);

} // namespace pulse::backtest
