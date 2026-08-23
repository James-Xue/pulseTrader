// kline_loader.cpp — Orchestrates candle loading for a backtest run (M29)

#include "backtest/KlineLoader.hpp"

#include "logging/Logger.hpp"

#include <algorithm>

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

    return merged;
}

} // namespace pulse::backtest
