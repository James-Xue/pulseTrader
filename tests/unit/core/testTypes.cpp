// testTypes.cpp — Unit tests for pulse/core/types.hpp, error.hpp, and config.hpp
//
// Test coverage:
//   1. Side::opposite() symmetry (Buy↔Sell)
//   2. Timestamp monotonicity and nanosecond resolution
//   3. Result<T> ok/value/error accessors
//   4. Default values for ExchangeConfig, AiConfig, RiskConfig

#include <gtest/gtest.h>

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/types.hpp"

using namespace pulse;

// ---------------------------------------------------------------------------
// types.hpp — Side and Timestamp
// ---------------------------------------------------------------------------

TEST(Side, OppositeIsSymmetric)
{
    // opposite() must be an involution: opposite(opposite(x)) == x
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite(Side::Sell), Side::Buy);
}

TEST(Timestamp, NowIsMonotonic)
{
    // Two successive calls must return non-decreasing timestamps
    const auto t1 = now();
    const auto t2 = now();
    EXPECT_LE(t1, t2);
}

TEST(Timestamp, NowHasNanosecondResolution)
{
    // Compile-time check: Timestamp must be backed by std::chrono::nanoseconds
    static_assert(
        std::is_same_v<Timestamp::duration, std::chrono::nanoseconds>, "Timestamp must have nanosecond resolution");
}

// ---------------------------------------------------------------------------
// types.hpp — MarketType and MarginMode
// ---------------------------------------------------------------------------

TEST(MarketType, ToStringSpot)
{
    EXPECT_STREQ(to_string(MarketType::Spot), "spot");
}

TEST(MarketType, ToStringFutures)
{
    EXPECT_STREQ(to_string(MarketType::Futures), "futures");
}

TEST(MarginMode, ToStringCross)
{
    EXPECT_STREQ(to_string(MarginMode::Cross), "cross");
}

TEST(MarginMode, ToStringIsolated)
{
    EXPECT_STREQ(to_string(MarginMode::Isolated), "isolated");
}

// ---------------------------------------------------------------------------
// error.hpp — Result<T>
// ---------------------------------------------------------------------------

TEST(Result, OkVariant)
{
    // A Result<int> constructed from a value must report ok() == true
    Result<int> r = 42;
    EXPECT_TRUE(ok(r));
    EXPECT_EQ(value(r), 42);
}

TEST(Result, ErrorVariant)
{
    // A Result<int> constructed from a PulseError must report ok() == false
    Result<int> r = PulseError{ ErrorCode::NetworkTimeout, "timed out" };
    EXPECT_FALSE(ok(r));
    EXPECT_EQ(error(r).code, ErrorCode::NetworkTimeout);
    EXPECT_EQ(error(r).message, "timed out");
}

TEST(Result, MutableValue)
{
    // value() on a mutable Result must return a writable reference
    Result<int> r = 10;
    value(r) = 20;
    EXPECT_EQ(value(r), 20);
}

// ---------------------------------------------------------------------------
// config.hpp — default values
// ---------------------------------------------------------------------------

TEST(PulseConfig, ExchangeDefaults)
{
    // ExchangeConfig must ship with Gate.io production defaults
    ExchangeConfig cfg;
    EXPECT_EQ(cfg.restBaseUrl, "https://api.gateio.ws");
    EXPECT_EQ(cfg.futuresWsUrl, "wss://fx-ws.gateio.ws/v4/ws/usdt");
    EXPECT_EQ(cfg.restTimeoutMs, 10'000u);
    EXPECT_EQ(cfg.maxRetries, 3u);
}

TEST(PulseConfig, AiDefaults)
{
    // AiConfig must default to Claude backend with 5-minute heartbeat
    AiConfig cfg;
    EXPECT_EQ(cfg.backend, "claude");
    EXPECT_EQ(cfg.heartbeatIntervalSec, 300u);
}

TEST(PulseConfig, RiskDefaults)
{
    // RiskConfig must default to conservative limits (2% daily, 5 orders/sec)
    RiskConfig cfg;
    EXPECT_DOUBLE_EQ(cfg.maxDailyDrawdown, 0.02);
    EXPECT_EQ(cfg.maxOrdersPerSec, 5u);
    EXPECT_DOUBLE_EQ(cfg.max_leverage, 10.0);
    EXPECT_DOUBLE_EQ(cfg.max_margin_used, 0.5);
}

TEST(PulseConfig, StrategyInstanceDefaults)
{
    // StrategyInstanceConfig must default to Spot market, 1x leverage, Cross margin
    StrategyInstanceConfig cfg;
    EXPECT_EQ(cfg.market_type, MarketType::Spot);
    EXPECT_DOUBLE_EQ(cfg.leverage, 1.0);
    EXPECT_EQ(cfg.margin_mode, MarginMode::Cross);
}

TEST(PulseConfig, DefaultMarketType)
{
    // PulseConfig must default to Spot market type
    PulseConfig cfg;
    EXPECT_EQ(cfg.default_market_type, MarketType::Spot);
}
