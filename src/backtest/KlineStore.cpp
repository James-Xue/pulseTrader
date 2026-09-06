// kline_store.cpp — Trailing kline accumulation + DB import (M33, A3)

#include "backtest/KlineStore.hpp"

#include "logging/Logger.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <string>

namespace pulse::backtest
{

namespace
{

constexpr std::int64_t kMinuteMs = 60'000;

/// Retry slack: shift the fetch window forward 10 minutes on a boundary
/// rejection (same knob as DailyKlineSync).
constexpr std::int64_t kRetrySlackMs = 10 * kMinuteMs;

/// Batch size for importFuturesFromDb writeBack chunks.
constexpr std::size_t kImportBatch = 5'000;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

KlineStore::KlineStore(IKlineSource &api, IKlineSource &sink)
    : m_api{ api }
    , m_sink{ sink }
{
}

// ---------------------------------------------------------------------------
// storeWindow
// ---------------------------------------------------------------------------

Result<KlineStoreStats> KlineStore::storeWindow(
    const std::string &symbol, MarketType market_type,
    std::int64_t from_ms, std::int64_t to_ms, std::int64_t now_ms)
{
    auto fetched = m_api.fetch(symbol, market_type, from_ms, to_ms);
    if (!ok(fetched))
    {
        // One retry with the window shifted forward (the "most recent N
        // points" boundary moves with time).
        PULSE_LOG_WARN("kline_store", "[{}] fetch failed ({}): retrying with "
                       "+10 min slack", symbol, error(fetched).message);
        fetched = m_api.fetch(symbol, market_type,
                              from_ms + kRetrySlackMs, to_ms + kRetrySlackMs);
    }
    if (!ok(fetched))
    {
        return PulseError{ ErrorCode::BacktestApiFetchFailed,
            "kline-store fetch failed for " + symbol + ": "
                + error(fetched).message };
    }

    auto candles = value(fetched);

    KlineStoreStats stats;
    stats.rows_fetched = candles.size();

    // Drop the forming candle: the futures candlestick layout marks every
    // row closed=true (no reliable closed flag), so a candle whose minute
    // has not elapsed yet is cut here instead.
    candles.erase(
        std::remove_if(candles.begin(), candles.end(),
            [now_ms](const market::Kline &k)
            {
                return k.open_time >= now_ms - kMinuteMs;
            }),
        candles.end());

    if (candles.empty())
    {
        PULSE_LOG_WARN("kline_store", "[{}] no finished candles in window",
                       symbol);
        return stats;
    }

    auto written = m_sink.writeBack(symbol, market_type, candles);
    if (!ok(written))
    {
        return PulseError{ ErrorCode::BacktestSqliteUnavailable,
            "kline-store writeBack failed for " + symbol + ": "
                + error(written).message };
    }

    stats.rows_new = value(written);
    stats.first_open_ms = candles.front().open_time;
    stats.last_open_ms = candles.back().open_time;
    return stats;
}

// ---------------------------------------------------------------------------
// importFuturesFromDb — one-shot jump-start from an existing kline_bars db
// ---------------------------------------------------------------------------

Result<std::size_t> importFuturesFromDb(
    const std::string &source_db_path,
    const std::vector<std::string> &symbols,
    IKlineSource &sink)
{
    try
    {
        SQLite::Database source(source_db_path, SQLite::OPEN_READWRITE);

        std::string sql = "SELECT symbol, open_time, close_time, open, high, "
                          "low, close, volume, closed "
                          "FROM kline_bars WHERE market_type = 'futures'";
        if (!symbols.empty())
        {
            sql += " AND symbol IN (";
            for (std::size_t i = 0; i < symbols.size(); ++i)
            {
                sql += (0 == i) ? "?" : ", ?";
            }
            sql += ")";
        }
        // Per-symbol ordering lets one batch carry one symbol for writeBack.
        sql += " ORDER BY symbol, open_time ASC";

        SQLite::Statement stmt(source, sql);
        for (std::size_t i = 0; i < symbols.size(); ++i)
        {
            stmt.bind(static_cast<int>(i + 1), symbols[i]);
        }

        std::size_t inserted_total = 0;
        std::string current_symbol;
        std::vector<market::Kline> batch;
        batch.reserve(kImportBatch);

        const auto flush = [&]() -> Result<std::size_t>
        {
            if (batch.empty())
            {
                return std::size_t{ 0 };
            }
            auto written = sink.writeBack(current_symbol, MarketType::Futures,
                                          batch);
            if (!ok(written))
            {
                return PulseError{ ErrorCode::BacktestSqliteUnavailable,
                    "kline-store import writeBack failed for "
                        + current_symbol + ": " + error(written).message };
            }
            const auto inserted = value(written);
            batch.clear();
            return inserted;
        };

        while (stmt.executeStep())
        {
            const std::string row_symbol = stmt.getColumn(0).getString();
            if (batch.empty())
            {
                current_symbol = row_symbol;
            }
            else if (row_symbol != current_symbol)
            {
                auto flushed = flush();
                if (!ok(flushed))
                {
                    return error(flushed);
                }
                inserted_total += value(flushed);
                current_symbol = row_symbol;
            }

            market::Kline k;
            k.open_time = stmt.getColumn(1).getInt64();
            k.close_time = stmt.getColumn(2).getInt64();
            k.open = stmt.getColumn(3).getDouble();
            k.high = stmt.getColumn(4).getDouble();
            k.low = stmt.getColumn(5).getDouble();
            k.close = stmt.getColumn(6).getDouble();
            k.volume = stmt.getColumn(7).getDouble();
            k.closed = (0 != stmt.getColumn(8).getInt());
            batch.push_back(k);

            if (batch.size() >= kImportBatch)
            {
                auto flushed = flush();
                if (!ok(flushed))
                {
                    return error(flushed);
                }
                inserted_total += value(flushed);
            }
        }

        auto tail = flush();
        if (!ok(tail))
        {
            return error(tail);
        }
        inserted_total += value(tail);
        return inserted_total;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ ErrorCode::BacktestSqliteUnavailable,
            "kline-store import failed to read " + source_db_path + ": "
                + e.what() };
    }
}

} // namespace pulse::backtest
