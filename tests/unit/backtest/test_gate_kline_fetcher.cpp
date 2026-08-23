// test_gate_kline_fetcher.cpp — pure-function tests (no network) for the
// Gate REST kline parsing/pagination helpers.

#include "backtest/GateKlineFetcher.hpp"

#include <gtest/gtest.h>

namespace pulse::backtest::test
{

// ---------------------------------------------------------------------------
// parseCandles — futures layout: array of OBJECTS {t, o, h, l, c, v}
// (probed live on 2026-08-23 — futures REST returns objects, not arrays)
// ---------------------------------------------------------------------------

TEST(GateKlineFetcherTest, ParseCandles_FuturesLayout)
{
    const nlohmann::json body = nlohmann::json::array({
        nlohmann::json::object({ { "t", "1700000000" }, { "o", "2370.6" },
                                 { "h", "2384.4" }, { "l", "2370.6" },
                                 { "c", "2379.3" }, { "v", "13560" } }),
        nlohmann::json::object({ { "t", "1700000060" }, { "o", "2379.3" },
                                 { "h", "2385.0" }, { "l", "2378.9" },
                                 { "c", "2380.1" }, { "v", "13600" } }),
    });

    const auto result = GateKlineFetcher::parseCandles(body, MarketType::Futures, 60'000);
    ASSERT_TRUE(ok(result));

    const auto &candles = value(result);
    ASSERT_EQ(2u, candles.size());

    EXPECT_EQ(1'700'000'000'000LL, candles[0].open_time);  // seconds → ms.
    EXPECT_EQ(1'700'000'060'000LL, candles[0].close_time); // + interval.
    EXPECT_DOUBLE_EQ(2370.6, candles[0].open);
    EXPECT_DOUBLE_EQ(2384.4, candles[0].high);
    EXPECT_DOUBLE_EQ(2370.6, candles[0].low);
    EXPECT_DOUBLE_EQ(2379.3, candles[0].close);
    EXPECT_DOUBLE_EQ(13560.0, candles[0].volume);
    EXPECT_TRUE(candles[0].closed);

    EXPECT_EQ(1'700'000'060'000LL, candles[1].open_time);
    EXPECT_EQ(1'700'000'120'000LL, candles[1].close_time);
}

// ---------------------------------------------------------------------------
// parseCandles — spot layout: array [ts, quote_vol, close, high, low, open,
// base_vol, closed] (probed live on 2026-08-23 — the volume column is index 6)
// ---------------------------------------------------------------------------

TEST(GateKlineFetcherTest, ParseCandles_SpotLayout)
{
    const nlohmann::json body = nlohmann::json::array({
        nlohmann::json::array({ "1700000000", "907.5", "3852.96", "3852.96",
                                "3850.10", "3850.50", "0.2355", "true" }),
    });

    const auto result = GateKlineFetcher::parseCandles(body, MarketType::Spot, 60'000);
    ASSERT_TRUE(ok(result));

    const auto &candles = value(result);
    ASSERT_EQ(1u, candles.size());
    EXPECT_DOUBLE_EQ(3850.50, candles[0].open);
    EXPECT_DOUBLE_EQ(3852.96, candles[0].close);
    EXPECT_DOUBLE_EQ(0.2355, candles[0].volume); // base_vol at [6] for spot.
}

TEST(GateKlineFetcherTest, ParseCandles_EmptyArray)
{
    const auto result = GateKlineFetcher::parseCandles(
        nlohmann::json::array(), MarketType::Futures, 60'000);
    ASSERT_TRUE(ok(result));
    EXPECT_TRUE(value(result).empty());
}

TEST(GateKlineFetcherTest, ParseCandles_NotArray_IsError)
{
    const auto result = GateKlineFetcher::parseCandles(
        nlohmann::json::object({ { "oops", 1 } }), MarketType::Futures, 60'000);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::BacktestApiFetchFailed, error(result).code);
}

TEST(GateKlineFetcherTest, ParseCandles_SkipsMalformedRows)
{
    // Row 1: missing keys. Row 3: non-numeric close. Row 4: wrong shape
    // (array under futures market). Only row 0 must survive.
    const nlohmann::json body = nlohmann::json::array({
        nlohmann::json::object({ { "t", "1700000000" }, { "o", "1" }, { "h", "2" },
                                 { "l", "3" }, { "c", "4" }, { "v", "5" } }),
        nlohmann::json::object({ { "t", "1700000060" }, { "o", "1" } }),
        nlohmann::json::object({ { "t", "1700000120" }, { "o", "1" }, { "h", "2" },
                                 { "l", "3" }, { "c", "nope" }, { "v", "5" } }),
        nlohmann::json::array({ "1700000180", "1", "2", "3", "4", "5", "6" }),
    });

    const auto result = GateKlineFetcher::parseCandles(body, MarketType::Futures, 60'000);
    ASSERT_TRUE(ok(result));
    ASSERT_EQ(1u, value(result).size());
    EXPECT_EQ(1'700'000'000'000LL, value(result)[0].open_time);
}

TEST(GateKlineFetcherTest, ParseCandles_NumericTimestamps)
{
    // Numeric timestamp + numeric OHLC, futures object layout.
    const nlohmann::json body = nlohmann::json::array({
        nlohmann::json::object({ { "t", 1700000000 }, { "o", 1 }, { "h", 2 },
                                 { "l", 3 }, { "c", 4 }, { "v", 5 } }),
    });

    const auto result = GateKlineFetcher::parseCandles(body, MarketType::Futures, 60'000);
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(1'700'000'000'000LL, value(result)[0].open_time);
}

// ---------------------------------------------------------------------------
// splitRange — window chunking
// ---------------------------------------------------------------------------

TEST(GateKlineFetcherTest, SplitRange_ExactMultiples)
{
    const auto chunks = GateKlineFetcher::splitRange(0, 1'000'000, 60'000, 4);
    ASSERT_EQ(5u, chunks.size());
    EXPECT_EQ(std::make_pair(0LL, 239'999LL), chunks[0]);
    EXPECT_EQ(std::make_pair(240'000LL, 479'999LL), chunks[1]);
    EXPECT_EQ(std::make_pair(960'000LL, 1'000'000LL), chunks[4]);
}

TEST(GateKlineFetcherTest, SplitRange_Remainder)
{
    const auto chunks = GateKlineFetcher::splitRange(0, 100'000, 60'000, 2);
    ASSERT_EQ(1u, chunks.size());
    EXPECT_EQ(std::make_pair(0LL, 100'000LL), chunks[0]);
}

TEST(GateKlineFetcherTest, SplitRange_InvalidInput_Empty)
{
    EXPECT_TRUE(GateKlineFetcher::splitRange(100, 0, 60'000, 4).empty());
    EXPECT_TRUE(GateKlineFetcher::splitRange(0, 100, 0, 4).empty());
    EXPECT_TRUE(GateKlineFetcher::splitRange(0, 100, 60'000, 0).empty());
}

// ---------------------------------------------------------------------------
// intervalToString
// ---------------------------------------------------------------------------

TEST(GateKlineFetcherTest, IntervalToString_Supported)
{
    EXPECT_EQ("1m", value(GateKlineFetcher::intervalToString(60'000)));
    EXPECT_EQ("5m", value(GateKlineFetcher::intervalToString(300'000)));
    EXPECT_EQ("1h", value(GateKlineFetcher::intervalToString(3'600'000)));
    EXPECT_EQ("1d", value(GateKlineFetcher::intervalToString(86'400'000)));
}

TEST(GateKlineFetcherTest, IntervalToString_Unknown_IsError)
{
    const auto result = GateKlineFetcher::intervalToString(123'456);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::BacktestRangeInvalid, error(result).code);
}

} // namespace pulse::backtest::test
