// test_kline_loader.cpp — KlineLoader orchestration + findKlineGaps, with a
// stubbed API source so no network is touched.

#include "backtest/KlineLoader.hpp"

#include <gtest/gtest.h>

namespace pulse::backtest::test
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

market::Kline makeCandle(std::int64_t open_ms, double close = 100.0)
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

/// Stub API source: serves a fixed candle map per requested range and counts
/// the calls so tests can assert gap-driven fetch behaviour.
class StubApiSource final : public IKlineSource
{
  public:
    std::vector<std::pair<std::int64_t, std::int64_t>> requested_ranges;
    std::vector<market::Kline> candles_to_serve;

    Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override
    {
        (void)symbol;
        (void)market_type;
        requested_ranges.emplace_back(from_ms, to_ms);

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
        return "stub-api";
    }
};

/// No-op local source: keeps rows pre-seeded and records writeBack calls.
class StubLocalSource final : public IKlineSource
{
  public:
    std::vector<market::Kline> rows;
    std::vector<market::Kline> written;

    Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override
    {
        (void)symbol;
        (void)market_type;
        std::vector<market::Kline> out;
        for (const auto &c : rows)
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
        return "stub-local";
    }

    Result<std::size_t> writeBack(
        const std::string &symbol, MarketType market_type,
        const std::vector<market::Kline> &candles) override
    {
        (void)symbol;
        (void)market_type;
        written = candles;
        return candles.size();
    }
};

// ---------------------------------------------------------------------------
// findKlineGaps — pure gap detection
// ---------------------------------------------------------------------------

TEST(KlineLoaderTest, FindGaps_EmptyRows_WholeWindowIsGap)
{
    const auto gaps = findKlineGaps({}, 1'000'000, 5'000'000, 60'000);
    ASSERT_EQ(1u, gaps.size());
    EXPECT_EQ(std::make_pair(1'000'000LL, 5'000'000LL), gaps[0]);
}

TEST(KlineLoaderTest, FindGaps_HeadMiddleTail)
{
    const std::vector<market::Kline> rows = {
        makeCandle(2'000'000), // 2000s
        makeCandle(2'060'000), // 2060s — contiguous with 2000s
        makeCandle(2'300'000), // 2300s — gap 2060 → 2300
    };

    // Window [1500, 5000]: head gap, middle gap, tail gap.
    const auto gaps = findKlineGaps(rows, 1'500'000, 5'000'000, 60'000);
    ASSERT_EQ(3u, gaps.size());
    EXPECT_EQ(std::make_pair(1'500'000LL, 1'940'000LL), gaps[0]); // head
    EXPECT_EQ(std::make_pair(2'120'000LL, 2'240'000LL), gaps[1]); // middle
    EXPECT_EQ(std::make_pair(2'360'000LL, 5'000'000LL), gaps[2]); // tail
}

TEST(KlineLoaderTest, FindGaps_NoGaps_Empty)
{
    const std::vector<market::Kline> rows = {
        makeCandle(1'000'000),
        makeCandle(1'060'000),
        makeCandle(1'120'000),
    };
    EXPECT_TRUE(findKlineGaps(rows, 1'000'000, 1'120'000, 60'000).empty());
}

// ---------------------------------------------------------------------------
// load — orchestration
// ---------------------------------------------------------------------------

TEST(KlineLoaderTest, Load_LocalOnly_NoApiCallWhenCovered)
{
    StubLocalSource local;
    local.rows = { makeCandle(1'000'000), makeCandle(1'060'000) };
    StubApiSource api;

    KlineLoader loader(&local, api);
    KlineLoadStats stats;
    KlineLoadRequest req;
    req.symbol = "ETH_USDT";
    req.market_type = MarketType::Futures;
    req.from_ms = 1'000'000;
    req.to_ms = 1'060'000;
    req.interval_ms = 60'000;
    req.api_backfill = true;

    const auto result = loader.load(req, stats);
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(2u, value(result).size());
    EXPECT_EQ(2u, stats.rows_sqlite);
    EXPECT_EQ(0u, stats.rows_api);
    EXPECT_EQ(0, stats.missing_range_count);
    EXPECT_TRUE(api.requested_ranges.empty());
}

TEST(KlineLoaderTest, Load_GapsTriggerApiCalls_AndMergeDedup)
{
    StubLocalSource local;
    local.rows = { makeCandle(1'000'000), makeCandle(1'120'000) }; // 2000s, 2120s
    StubApiSource api;
    api.candles_to_serve = {
        makeCandle(1'060'000), // fills the gap — but ALSO duplicates a local row
        makeCandle(1'120'000),
    };

    KlineLoader loader(&local, api);
    KlineLoadStats stats;
    KlineLoadRequest req;
    req.symbol = "ETH_USDT";
    req.market_type = MarketType::Futures;
    req.from_ms = 1'000'000;
    req.to_ms = 1'120'000;
    req.interval_ms = 60'000;
    req.api_backfill = true;
    req.cache_writeback = false;

    const auto result = loader.load(req, stats);
    ASSERT_TRUE(ok(result));
    const auto &candles = value(result);

    // 1'000'000 (local), 1'060'000 (api), 1'120'000 (local wins dedup) →
    // exactly 3 candles, sorted ascending.
    ASSERT_EQ(3u, candles.size());
    EXPECT_EQ(1'000'000LL, candles[0].open_time);
    EXPECT_EQ(1'060'000LL, candles[1].open_time);
    EXPECT_EQ(1'120'000LL, candles[2].open_time);
    EXPECT_EQ(1u, stats.rows_api);
    EXPECT_EQ(2u, stats.rows_sqlite);
    EXPECT_EQ(1, stats.missing_range_count);
    EXPECT_EQ(1u, api.requested_ranges.size());
    EXPECT_EQ(std::make_pair(1'060'000LL, 1'060'000LL), api.requested_ranges[0]);
}

TEST(KlineLoaderTest, Load_ApiFailure_DegradesWithWarning)
{
    StubLocalSource local; // empty local
    StubApiSource api;     // serves nothing, but requested ranges are recorded

    KlineLoader loader(&local, api);
    KlineLoadStats stats;
    KlineLoadRequest req;
    req.symbol = "ETH_USDT";
    req.market_type = MarketType::Futures;
    req.from_ms = 1'000'000;
    req.to_ms = 1'120'000;
    req.interval_ms = 60'000;
    req.api_backfill = true;

    const auto result = loader.load(req, stats);
    // Local empty + API served nothing → BacktestNoCandles.
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::BacktestNoCandles, error(result).code);
    EXPECT_EQ(1, stats.missing_range_count);
}

TEST(KlineLoaderTest, Load_WriteBack_OnlyWhenApiRowsExist)
{
    StubLocalSource local;
    local.rows = { makeCandle(1'000'000), makeCandle(1'060'000) };
    StubApiSource api;
    api.candles_to_serve = { makeCandle(1'120'000) };

    KlineLoader loader(&local, api);
    KlineLoadStats stats;
    KlineLoadRequest req;
    req.symbol = "ETH_USDT";
    req.market_type = MarketType::Futures;
    req.from_ms = 1'000'000;
    req.to_ms = 1'120'000;
    req.interval_ms = 60'000;
    req.api_backfill = true;
    req.cache_writeback = true;

    const auto result = loader.load(req, stats);
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(3u, value(result).size());
    // writeBack called with the merged set.
    ASSERT_EQ(3u, local.written.size());
    EXPECT_EQ(1'000'000LL, local.written[0].open_time);
}

TEST(KlineLoaderTest, Load_InvalidRange_IsError)
{
    StubLocalSource local;
    StubApiSource api;
    KlineLoader loader(&local, api);
    KlineLoadStats stats;
    KlineLoadRequest req;
    req.symbol = "ETH_USDT";
    req.from_ms = 2'000'000;
    req.to_ms = 1'000'000;

    const auto result = loader.load(req, stats);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::BacktestRangeInvalid, error(result).code);
}

TEST(KlineLoaderTest, Load_ApiBackfillDisabled_NoApiCalls)
{
    StubLocalSource local;
    StubApiSource api;
    KlineLoader loader(&local, api);
    KlineLoadStats stats;
    KlineLoadRequest req;
    req.symbol = "ETH_USDT";
    req.market_type = MarketType::Futures;
    req.from_ms = 1'000'000;
    req.to_ms = 1'120'000;
    req.interval_ms = 60'000;
    req.api_backfill = false;

    const auto result = loader.load(req, stats);
    ASSERT_FALSE(ok(result)); // local empty + no API → NoCandles.
    EXPECT_TRUE(api.requested_ranges.empty());
}

} // namespace pulse::backtest::test
