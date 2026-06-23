// test_prompt_builder.cpp — Unit tests for PromptBuilder (Layer 4)
//
// Tests:
//   1. System prompt contains JSON schema
//   2. User prompt includes market data
//   3. User prompt includes/excludes social data
//   4. Empty klines handled gracefully
//   5. Current params are included in user prompt

#include "ai/PromptBuilder.hpp"

#include <gtest/gtest.h>

using namespace pulse::ai;
using namespace pulse::market;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// 1. System prompt contains JSON schema
// ---------------------------------------------------------------------------
TEST(PromptBuilder, SystemPromptContainsSchema)
{
    PromptBuilder builder;
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    StrategyParams params;

    auto [system, user] = builder.build(snapshot, "", "", params);

    // System prompt must contain key schema elements
    EXPECT_NE(system.find("sentiment"), std::string::npos);
    EXPECT_NE(system.find("bullish"), std::string::npos);
    EXPECT_NE(system.find("bearish"), std::string::npos);
    EXPECT_NE(system.find("confidence"), std::string::npos);
    EXPECT_NE(system.find("param_deltas"), std::string::npos);
    EXPECT_NE(system.find("JSON"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 2. User prompt includes market data
// ---------------------------------------------------------------------------
TEST(PromptBuilder, UserPromptIncludesMarketData)
{
    PromptBuilder builder;
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    snapshot.ticker.last = 65000.0;
    snapshot.ticker.bid = 64999.0;
    snapshot.ticker.ask = 65001.0;
    snapshot.ticker.change_pct = 2.5;
    snapshot.ticker.volume_24h = 15000.0;
    StrategyParams params;

    auto [system, user] = builder.build(snapshot, "", "", params);

    EXPECT_NE(user.find("BTC_USDT"), std::string::npos);
    EXPECT_NE(user.find("65000"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 3. User prompt includes social data when provided
// ---------------------------------------------------------------------------
TEST(PromptBuilder, UserPromptIncludesSocialData)
{
    PromptBuilder builder;
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    StrategyParams params;

    std::string tweets = "BTC breaking out! Bullish momentum building.";
    std::string news = "Bitcoin ETF approved by SEC.";

    auto [system, user] = builder.build(snapshot, tweets, news, params);

    EXPECT_NE(user.find("BTC breaking out"), std::string::npos);
    EXPECT_NE(user.find("Bitcoin ETF"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 4. User prompt handles empty social data
// ---------------------------------------------------------------------------
TEST(PromptBuilder, EmptySocialDataHandled)
{
    PromptBuilder builder;
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "ETH_USDT";
    StrategyParams params;

    auto [system, user] = builder.build(snapshot, "", "", params);

    // Should still produce a valid prompt without social data
    EXPECT_FALSE(user.empty());
    EXPECT_NE(user.find("ETH_USDT"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 5. User prompt includes current parameter values
// ---------------------------------------------------------------------------
TEST(PromptBuilder, UserPromptIncludesParams)
{
    PromptBuilder builder;
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    StrategyParams params;
    params.order_quantity.store(0.005, std::memory_order_release);
    params.ema_fast_period.store(12.0, std::memory_order_release);

    auto [system, user] = builder.build(snapshot, "", "", params);

    // Params should be present in the user prompt
    EXPECT_NE(user.find("order_quantity"), std::string::npos);
    EXPECT_NE(user.find("ema_fast_period"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 6. Klines are included when available
// ---------------------------------------------------------------------------
TEST(PromptBuilder, KlinesIncluded)
{
    PromptBuilder builder;
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";

    Kline k;
    k.open = 64000.0;
    k.high = 65500.0;
    k.low = 63800.0;
    k.close = 65000.0;
    k.volume = 100.0;
    k.closed = true;
    snapshot.klines.push_back(k);

    StrategyParams params;
    auto [system, user] = builder.build(snapshot, "", "", params);

    // Kline data should appear in the prompt
    EXPECT_NE(user.find("65000"), std::string::npos);
}
