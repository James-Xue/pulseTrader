// warmup_seeder.cpp — WarmupSeeder implementation (M30 startup preload)

#include "backtest/WarmupSeeder.hpp"

#include "logging/Logger.hpp"

#include <chrono>

namespace pulse::backtest
{

namespace
{

/// 1m bar length in ms (the seed window is 1m candles only).
constexpr std::int64_t kMinuteMs = 60'000;

} // anonymous namespace

WarmupSeeder::WarmupSeeder(IKlineSource *sqlite, IKlineSource &api)
    : m_loader{ sqlite, api }
{
}

std::size_t WarmupSeeder::seed(market::MarketFeed &feed,
                               const std::vector<std::string> &symbols,
                               MarketType market_type,
                               std::size_t count)
{
    if (symbols.empty())
    {
        return 0;
    }

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto from_ms = now_ms - static_cast<std::int64_t>(count) * kMinuteMs;

    std::size_t total{ 0 };
    for (const auto &symbol : symbols)
    {
        // cache_writeback=false is deliberate: seeding must NOT persist into
        // kline_bars — M30 retires self-recorded futures klines (REST is the
        // live source now). The backtest CLI keeps its explicit writeback.
        const KlineLoadRequest req{
            .symbol           = symbol,
            .market_type      = market_type,
            .from_ms          = from_ms,
            .to_ms            = now_ms,
            .interval_ms      = kMinuteMs,
            .api_backfill     = true,
            .cache_writeback  = false,
        };

        KlineLoadStats stats;
        auto candles = m_loader.load(req, stats);
        if (!ok(candles))
        {
            PULSE_LOG_WARN("backtest", "[{}] warmup preload skipped (load failed: {}) — falling back to live warmup",
                           symbol, error(candles).message);
            continue;
        }
        if (value(candles).empty())
        {
            PULSE_LOG_WARN("backtest", "[{}] warmup preload skipped (no candles in window) — falling back to live warmup",
                           symbol);
            continue;
        }

        auto &buffer = feed.getKlineBuffer(symbol);
        std::size_t seeded{ 0 };
        for (auto &c : value(candles))
        {
            // Drop the forming (current, unclosed) candle — its open_time +
            // interval may still be in the future, and the strategy loop
            // only fires onKline for closed candles.
            if (c.open_time + kMinuteMs > now_ms)
            {
                continue;
            }
            // GateKlineFetcher's futures parser leaves `closed` unset
            // (defaults false); the strategy loop gates on it, so force it.
            c.closed = true;
            buffer.push(c);
            ++seeded;
        }

        total += seeded;
        PULSE_LOG_INFO("backtest", "warmup preload: seeded {} candles into {} (sqlite {} + api {})",
                       seeded, symbol, stats.rows_sqlite, stats.rows_api);
    }

    return total;
}

} // namespace pulse::backtest
