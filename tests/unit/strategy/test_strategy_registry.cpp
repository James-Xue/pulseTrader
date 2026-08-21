// test_strategy_registry.cpp — Unit tests for the strategy registry + fallback
//
// Contract under test:
//   - Builtin names resolve to their concrete classes with legacy ids.
//   - Unregistered names fall back to a PASSIVE UnifiedScalper (never emits
//     signals) with an id derived from the TOML name — never skipped.
//   - Duplicate registration is rejected (first registration wins).

#include "strategy/StrategyRegistry.hpp"
#include "strategy/scalping/MeanReversionScalper.hpp"
#include "strategy/scalping/MomentumScalper.hpp"
#include "strategy/scalping/OrderBookScalper.hpp"
#include "strategy/scalping/SuperTrendScalper.hpp"
#include "strategy/scalping/UnifiedScalper.hpp"

#include "exchange/GateRestClient.hpp"
#include "market/MarketFeed.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace pulse;
using namespace pulse::strategy;

namespace
{

static StrategyContext make_ctx(const std::string &name, const std::string &symbol)
{
    StrategyContext ctx;
    ctx.config.name = name;
    ctx.config.symbol = symbol;
    return ctx;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Builtin registration
// ---------------------------------------------------------------------------

TEST(StrategyRegistry, CreatesMomentumScalper)
{
    const auto registry = makeBuiltinStrategyRegistry();
    auto strat = registry.create("momentum_scalper", make_ctx("momentum_scalper", "BTC_USDT"));
    ASSERT_NE(nullptr, strat);
    EXPECT_NE(nullptr, dynamic_cast<MomentumScalper *>(strat.get()));
    EXPECT_EQ("MomentumScalper", strat->name());
    EXPECT_EQ("momentum_scalper_BTC_USDT", strat->id());
}

TEST(StrategyRegistry, CreatesMeanReversionScalper)
{
    const auto registry = makeBuiltinStrategyRegistry();
    auto strat = registry.create("mean_reversion_scalper", make_ctx("mean_reversion_scalper", "XAUUSD"));
    ASSERT_NE(nullptr, strat);
    EXPECT_NE(nullptr, dynamic_cast<MeanReversionScalper *>(strat.get()));
    EXPECT_EQ("mean_reversion_scalper_XAUUSD", strat->id());
}

TEST(StrategyRegistry, CreatesSuperTrendScalper)
{
    const auto registry = makeBuiltinStrategyRegistry();
    auto strat = registry.create("supertrend_scalper", make_ctx("supertrend_scalper", "ETH_USDT"));
    ASSERT_NE(nullptr, strat);
    EXPECT_NE(nullptr, dynamic_cast<SuperTrendScalper *>(strat.get()));
    EXPECT_EQ("supertrend_scalper_ETH_USDT", strat->id());
}

TEST(StrategyRegistry, CreatesOrderBookScalper)
{
    // OrderBookScalper must be explicitly registered — it must NEVER fall
    // into the passive kline fallback (that would silently change behavior).
    const auto registry = makeBuiltinStrategyRegistry();
    auto strat = registry.create("orderbook_scalper", make_ctx("orderbook_scalper", "BTC_USDT"));
    ASSERT_NE(nullptr, strat);
    EXPECT_NE(nullptr, dynamic_cast<OrderBookScalper *>(strat.get()));
    EXPECT_EQ("orderbook_scalper_BTC_USDT", strat->id());
}

// ---------------------------------------------------------------------------
// Fallback (unknown names)
// ---------------------------------------------------------------------------

TEST(StrategyRegistry, UnknownNameFallsBackToUnifiedScalper)
{
    const auto registry = makeBuiltinStrategyRegistry();
    auto strat = registry.create("bogus_scalper", make_ctx("bogus_scalper", "BTC_USDT"));
    ASSERT_NE(nullptr, strat); // Never nullptr — fallback, not skip.
    EXPECT_NE(nullptr, dynamic_cast<UnifiedScalper *>(strat.get()));
    EXPECT_EQ("UnifiedScalper", strat->name());
    EXPECT_EQ("bogus_scalper_BTC_USDT", strat->id());
}

TEST(StrategyRegistry, FallbackIsPassive)
{
    // A passive fallback never emits signals — even with a live feed and
    // enough candles (default evaluateEntry returns nullopt).
    struct FeedHarness
    {
        exchange::GateRestClient rest;
        market::MarketFeed feed;
        explicit FeedHarness(MarketType mt)
            : rest{ ExchangeConfig{}, mt }
            , feed{ nullptr, rest, mt }
        {
        }
    };
    FeedHarness harness{ MarketType::Futures };

    market::Kline k;
    k.open = 100.0;
    k.high = 102.0;
    k.low = 98.0;
    k.close = 101.0;
    k.closed = true;
    harness.feed.getKlineBuffer("BTC_USDT").push(k);
    harness.feed.getKlineBuffer("BTC_USDT").push(k);

    const auto registry = makeBuiltinStrategyRegistry();
    auto ctx = make_ctx("bogus_scalper", "BTC_USDT");
    ctx.market_feed = &harness.feed;
    auto strat = registry.create("bogus_scalper", ctx);
    strat->params().min_confidence.store(0.0, std::memory_order_release);

    std::vector<TradingSignal> received;
    strat->setSignalCallback([&](const TradingSignal &s)
        {
            received.push_back(s);
        });

    market::Kline trigger;
    trigger.closed = true;
    strat->onKline(trigger);

    EXPECT_TRUE(received.empty());
}

// ---------------------------------------------------------------------------
// Custom registration & duplicates
// ---------------------------------------------------------------------------

TEST(StrategyRegistry, CustomRegisteredStrategyResolvable)
{
    StrategyRegistry registry;
    EXPECT_TRUE(registry.registerStrategy<MomentumScalper>("custom_momentum"));
    auto strat = registry.create("custom_momentum", make_ctx("custom_momentum", "BTC_USDT"));
    ASSERT_NE(nullptr, strat);
    EXPECT_NE(nullptr, dynamic_cast<MomentumScalper *>(strat.get()));
    EXPECT_EQ("momentum_scalper_BTC_USDT", strat->id()); // id prefix is the class's own
}

TEST(StrategyRegistry, DuplicateRegistrationRejected)
{
    StrategyRegistry registry;
    EXPECT_TRUE(registry.registerStrategy<MomentumScalper>("dup_scalper"));
    // Second registration of the same name is refused — first one wins.
    EXPECT_FALSE(registry.registerStrategy<SuperTrendScalper>("dup_scalper"));

    // Still resolves to the first-registered type.
    auto strat = registry.create("dup_scalper", make_ctx("dup_scalper", "BTC_USDT"));
    EXPECT_NE(nullptr, dynamic_cast<MomentumScalper *>(strat.get()));
}

TEST(StrategyRegistry, RegisteredNamesListed)
{
    const auto registry = makeBuiltinStrategyRegistry();
    const auto names = registry.registeredNames();
    EXPECT_EQ(4u, names.size());
    EXPECT_NE(names.end(), std::find(names.begin(), names.end(), "momentum_scalper"));
    EXPECT_NE(names.end(), std::find(names.begin(), names.end(), "orderbook_scalper"));
    EXPECT_NE(names.end(), std::find(names.begin(), names.end(), "mean_reversion_scalper"));
    EXPECT_NE(names.end(), std::find(names.begin(), names.end(), "supertrend_scalper"));
}
