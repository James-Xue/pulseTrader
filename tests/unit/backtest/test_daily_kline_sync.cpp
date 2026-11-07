// test_daily_kline_sync.cpp — DailyKlineSync unit tests (M31)

#include "backtest/DailyKlineSync.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::backtest;

namespace pulse::backtest::test
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

market::Kline dk_makeCandle(std::int64_t open_ms, double close = 100.0)
{
    market::Kline k;
    k.open_time = open_ms;
    k.close_time = open_ms + 60'000;
    k.open = close;
    k.high = close + 1.0;
    k.low = close - 1.0;
    k.close = close;
    k.volume = 10.0;
    k.closed = true;
    return k;
}

/// Records every fetch range and writeBack batch (like test_kline_loader's
/// stubs, but namespaced to avoid ODR clashes).
class SyncApiSource final : public IKlineSource
{
  public:
    std::vector<market::Kline> candles_to_serve;
    bool fail_fetch{ false };
    int fetch_calls{ 0 };

    Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override
    {
        (void)symbol;
        (void)market_type;
        ++fetch_calls;
        if (fail_fetch)
        {
            return PulseError{ ErrorCode::NetworkTimeout, "stub fetch failure" };
        }
        std::vector<market::Kline> out;
        for (const auto &c : candles_to_serve)
        {
            if (from_ms <= c.open_time && c.open_time <= to_ms)
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string description() const override
    {
        return "sync-stub-api";
    }
};

/// Records writeBack calls so tests can assert persistence happened.
class SyncLocalSource final : public IKlineSource
{
  public:
    std::vector<market::Kline> rows;
    std::size_t written_total{ 0 };
    int writeback_calls{ 0 };

    Result<std::vector<market::Kline>> fetch(
        const std::string &, MarketType, std::int64_t, std::int64_t) override
    {
        return rows;
    }

    Result<std::size_t> writeBack(
        const std::string &, MarketType,
        const std::vector<market::Kline> &candles) override
    {
        ++writeback_calls;
        written_total += candles.size();
        return candles.size();
    }

    std::string description() const override
    {
        return "sync-stub-local";
    }
};

/// Default 1m window helper: now-ish range so stub candles are served.
std::int64_t dk_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Wait until the worker has finished a pending sync (or timed out).
void wait_for_idle(DailyKlineSync &sync, std::size_t spin_ms = 500)
{
    // Best-effort: the sync runs on a worker thread; the day-boundary tests
    // use empty symbol lists so syncOnce returns in microseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(spin_ms));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(DailyKlineSync, FirstTickFiresImmediately)
{
    SyncApiSource spot_api, futures_api;
    SyncLocalSource local;
    std::mutex rest_mutex;

    // Empty symbol lists → syncOnce is a no-op; the point is the day-key
    // fires on the FIRST tick (m_dayStartSec starts at 0).
    DailyKlineSync sync(&local, spot_api, futures_api, nullptr, rest_mutex,
                        {}, {}, {}, 8);

    // Can't observe the worker directly; just ensure no crash and the
    // second tick within the same day does not crash either.
    const auto t = dk_now_ms();
    sync.tick(t);
    sync.tick(t + 1000); // Same day.
    wait_for_idle(sync);
}

TEST(DailyKlineSync, SameDayDoesNotRefire)
{
    SyncApiSource spot_api, futures_api;
    SyncLocalSource local;
    std::mutex rest_mutex;

    DailyKlineSync sync(&local, spot_api, futures_api, nullptr, rest_mutex,
                        { "ETH_USDT" }, {}, {}, 8);

    const auto t = dk_now_ms();
    sync.tick(t);
    sync.tick(t + 60'000);      // Same UTC day.
    sync.tick(t + 3'600'000);   // Same UTC day (hour later).
    wait_for_idle(sync);

    // A same-day refire would fetch again; with one symbol served we get
    // exactly one fetch per symbol per fire.
    EXPECT_EQ(1, spot_api.fetch_calls);
}

TEST(DailyKlineSync, DayBoundaryRefires)
{
    SyncApiSource spot_api, futures_api;
    SyncLocalSource local;
    std::mutex rest_mutex;

    DailyKlineSync sync(&local, spot_api, futures_api, nullptr, rest_mutex,
                        { "ETH_USDT" }, {}, {}, 8);

    // 2026-08-23 04:00 UTC (Beijing 12:00) — after the 08:00 boundary.
    const std::int64_t day1 = 1787428800000LL;
    sync.tick(day1);
    wait_for_idle(sync);
    EXPECT_EQ(1, spot_api.fetch_calls);

    // Next UTC day (2026-08-24 04:00 UTC) — must refire.
    sync.tick(day1 + 86'400'000);
    wait_for_idle(sync);
    EXPECT_EQ(2, spot_api.fetch_calls);
}

TEST(DailyKlineSync, SyncsAndWritesBack)
{
    SyncApiSource spot_api, futures_api;
    SyncLocalSource local;
    std::mutex rest_mutex;

    const auto now = dk_now_ms();
    for (std::size_t i = 0; i < 500; ++i)
    {
        spot_api.candles_to_serve.push_back(
            dk_makeCandle(now - static_cast<std::int64_t>(500 - i) * 60'000));
    }

    DailyKlineSync sync(&local, spot_api, futures_api, nullptr, rest_mutex,
                        { "ETH_USDT" }, {}, {}, 8);

    sync.tick(now);
    wait_for_idle(sync);

    EXPECT_EQ(1, spot_api.fetch_calls);
    EXPECT_EQ(1, local.writeback_calls);
    EXPECT_EQ(500u, local.written_total);
}

TEST(DailyKlineSync, FetchFailureDoesNotThrow)
{
    SyncApiSource spot_api, futures_api;
    SyncLocalSource local;
    std::mutex rest_mutex;

    spot_api.fail_fetch = true;

    DailyKlineSync sync(&local, spot_api, futures_api, nullptr, rest_mutex,
                        { "ETH_USDT" }, {}, {}, 8);

    EXPECT_NO_THROW(sync.tick(dk_now_ms()));
    wait_for_idle(sync);
    EXPECT_EQ(0, local.writeback_calls); // Nothing to write.
}

TEST(DailyKlineSync, CfdSkippedWhenNull)
{
    SyncApiSource spot_api, futures_api;
    SyncLocalSource local;
    std::mutex rest_mutex;

    // cfd_api == nullptr with non-empty cfd_symbols must be a safe no-op.
    DailyKlineSync sync(&local, spot_api, futures_api, nullptr, rest_mutex,
                        {}, {}, { "XAUUSD" }, 8);

    EXPECT_NO_THROW(sync.tick(dk_now_ms()));
    wait_for_idle(sync);
}

TEST(DailyKlineSync, EmptySymbolsNoOp)
{
    SyncApiSource spot_api, futures_api;
    SyncLocalSource local;
    std::mutex rest_mutex;

    DailyKlineSync sync(&local, spot_api, futures_api, nullptr, rest_mutex,
                        {}, {}, {}, 8);

    sync.tick(dk_now_ms());
    wait_for_idle(sync);
    EXPECT_EQ(0, spot_api.fetch_calls);
    EXPECT_EQ(0, futures_api.fetch_calls);
    EXPECT_EQ(0, local.writeback_calls);
}

} // namespace pulse::backtest::test
