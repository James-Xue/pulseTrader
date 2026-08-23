// kline_loader.cpp — Orchestrates candle loading for a backtest run (M29)

#include "backtest/KlineLoader.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// findKlineGaps — pure gap detection over sorted candles
// ---------------------------------------------------------------------------

std::vector<std::pair<std::int64_t, std::int64_t>> findKlineGaps(
    const std::vector<market::Kline> &rows,
    std::int64_t from_ms, std::int64_t to_ms, std::int64_t interval_ms)
{
    std::vector<std::pair<std::int64_t, std::int64_t>> gaps;
    if (to_ms < from_ms || interval_ms <= 0)
    {
        return gaps;
    }

    if (rows.empty())
    {
        gaps.emplace_back(from_ms, to_ms);
        return gaps;
    }

    // Gap before the first candle.
    if (rows.front().open_time > from_ms)
    {
        gaps.emplace_back(from_ms, rows.front().open_time - interval_ms);
    }

    // Gaps between consecutive candles.
    for (std::size_t i = 0; i + 1 < rows.size(); ++i)
    {
        const std::int64_t expected_next = rows[i].open_time + interval_ms;
        if (rows[i + 1].open_time > expected_next)
        {
            gaps.emplace_back(expected_next, rows[i + 1].open_time - interval_ms);
        }
    }

    // Gap after the last candle.
    if (rows.back().open_time < to_ms)
    {
        gaps.emplace_back(rows.back().open_time + interval_ms, to_ms);
    }

    return gaps;
}

// ---------------------------------------------------------------------------
// sanitizeCandles — data-quality guard for recorder glitches
//
// The live recorder has captured implausible bars (e.g. an ETH 1m candle
// with close 76403.9 next to 2371 neighbours on 2026-08-21). Such rows
// corrupt ATR/EMA state for the whole warmup, so drop them before replay:
//   - non-positive OHLCV or inconsistent high/low
//   - a relative jump > 25% from the previous close (1m bars never move
//     that far even in flash moves; the 08-21 glitch produced a 32x jump
//     and a follow-up -32% bar that must also be caught)
// Dropped bars are counted and surfaced in the load warnings.
// ---------------------------------------------------------------------------

namespace
{

constexpr double kMaxRelativeJump = 0.25; ///< |c[i]-c[i-1]| / c[i-1] beyond this → drop.

bool looksPlausible(const market::Kline &k)
{
    return k.open > 0.0 && k.high > 0.0 && k.low > 0.0 && k.close > 0.0
        && k.volume >= 0.0
        && k.high >= std::max(k.open, k.close)
        && k.low <= std::min(k.open, k.close);
}

} // anonymous namespace

std::vector<market::Kline> sanitizeCandles(std::vector<market::Kline> candles,
                                           KlineLoadStats &stats)
{
    std::vector<market::Kline> clean;
    clean.reserve(candles.size());
    std::optional<double> prev_close;

    for (auto &k : candles)
    {
        bool drop = !looksPlausible(k);
        if (!drop && prev_close.has_value() && *prev_close > 0.0)
        {
            const double jump = std::abs(k.close - *prev_close) / *prev_close;
            drop = jump > kMaxRelativeJump;
        }

        if (drop)
        {
            stats.warnings.push_back("dropped implausible candle at "
                + std::to_string(k.open_time) + " (close " + std::to_string(k.close) + ")");
            continue;
        }
        prev_close = k.close;
        clean.push_back(std::move(k));
    }

    if (clean.size() != candles.size())
    {
        stats.warnings.push_back("data sanitizer dropped "
            + std::to_string(candles.size() - clean.size()) + " implausible candle(s)");
    }
    return clean;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

KlineLoader::KlineLoader(IKlineSource *sqlite, IKlineSource &api)
    : m_sqlite{ sqlite }
    , m_api{ api }
{
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

Result<std::vector<market::Kline>> KlineLoader::load(
    const KlineLoadRequest &req, KlineLoadStats &stats)
{
    if (req.to_ms < req.from_ms)
    {
        return PulseError{ ErrorCode::BacktestRangeInvalid,
            "Backtest window invalid: from " + std::to_string(req.from_ms)
                + " > to " + std::to_string(req.to_ms) };
    }

    std::vector<market::Kline> merged;

    // 1. Local data first.
    if (m_sqlite)
    {
        auto local = m_sqlite->fetch(req.symbol, req.market_type, req.from_ms, req.to_ms);
        if (!ok(local))
        {
            PULSE_LOG_WARN("backtest", "SQLite fetch degraded: {}", error(local).message);
            stats.warnings.push_back("SQLite fetch degraded: " + error(local).message);
        }
        else
        {
            merged = value(local);
            stats.rows_sqlite = merged.size();
        }
    }

    // 2. API backfill of missing ranges.
    if (req.api_backfill)
    {
        const auto gaps = findKlineGaps(merged, req.from_ms, req.to_ms, req.interval_ms);
        stats.missing_range_count = static_cast<int>(gaps.size());

        for (const auto &[gap_from, gap_to] : gaps)
        {
            if (gap_to < gap_from)
            {
                continue; // Degenerate edge gap (e.g. first candle at from_ms).
            }
            auto remote = m_api.fetch(req.symbol, req.market_type, gap_from, gap_to);
            if (!ok(remote))
            {
                stats.warnings.push_back(
                    "API fetch failed for [" + std::to_string(gap_from) + ", "
                    + std::to_string(gap_to) + "]: " + error(remote).message);
                PULSE_LOG_WARN("backtest", "API backfill gap [{}, {}] failed: {}",
                               gap_from, gap_to, error(remote).message);
                continue;
            }
            auto fetched = value(remote);
            stats.rows_api += fetched.size();
            merged.insert(merged.end(), std::make_move_iterator(fetched.begin()),
                          std::make_move_iterator(fetched.end()));
        }
    }

    // 3. Sort + dedup (local rows were pushed first, so they win ties).
    std::sort(merged.begin(), merged.end(),
        [](const market::Kline &a, const market::Kline &b)
        {
            return a.open_time < b.open_time;
        });
    merged.erase(std::unique(merged.begin(), merged.end(),
                    [](const market::Kline &a, const market::Kline &b)
                    {
                        return a.open_time == b.open_time;
                    }),
                 merged.end());

    stats.rows_total = merged.size();

    // 4. Cache write-back.
    if (req.cache_writeback && m_sqlite && !merged.empty() && 0 < stats.rows_api)
    {
        auto written = m_sqlite->writeBack(req.symbol, req.market_type, merged);
        if (ok(written))
        {
            PULSE_LOG_INFO("backtest", "Cached {} klines back to sqlite",
                           value(written));
        }
        else
        {
            stats.warnings.push_back("writeBack failed: " + error(written).message);
        }
    }

    if (merged.empty())
    {
        return PulseError{ ErrorCode::BacktestNoCandles,
            "No candles available for " + req.symbol + " in ["
            + std::to_string(req.from_ms) + ", " + std::to_string(req.to_ms) + "]" };
    }

    // 5. Data-quality sanitizing (recorder glitches corrupt strategy state).
    merged = sanitizeCandles(std::move(merged), stats);
    if (merged.empty())
    {
        return PulseError{ ErrorCode::BacktestNoCandles,
            "All candles for " + req.symbol + " were dropped as implausible" };
    }

    stats.rows_total = merged.size();
    return merged;
}

} // namespace pulse::backtest
