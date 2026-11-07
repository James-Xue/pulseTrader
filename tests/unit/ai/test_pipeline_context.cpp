// test_pipeline_context.cpp — EngineSnapshotCollector::fromSources (pure path)

#include "ai/EngineSnapshotCollector.hpp"

#include "strategy/StrategyParams.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::ai;
using namespace pulse::market;
using namespace pulse::strategy;

namespace
{

Ticker make_ticker(const std::string &symbol, double last)
{
    Ticker t;
    t.symbol = symbol;
    t.last = last;
    t.bid = last - 1.0;
    t.ask = last + 1.0;
    t.volume_24h = 1000.0;
    return t;
}

std::vector<StrategyHandle> make_handles(std::vector<StrategyParams> &params)
{
    std::vector<StrategyHandle> handles;
    for (std::size_t i = 0; i < params.size(); ++i)
    {
        handles.push_back({ "strategy_" + std::to_string(i), "Scalper",
                            i == 0 ? "BTC_USDT" : "ETH_USDT", "futures",
                            &params[i] });
    }
    return handles;
}

} // anonymous namespace

TEST(PipelineContext, CollectorBuildsSnapshotFromRealCache)
{
    TickerCache cache;
    cache.update("BTC_USDT", make_ticker("BTC_USDT", 65000.0));
    cache.update("ETH_USDT", make_ticker("ETH_USDT", 3500.0));

    std::vector<StrategyParams> params(2);
    auto handles = make_handles(params);

    const auto ctx = EngineSnapshotCollector::fromSources(
        &cache,
        [](const std::string &symbol)
        {
            market::Kline k;
            k.open_time = 1000;
            k.close = symbol == "BTC_USDT" ? 65001.0 : 0.0;
            k.closed = true;
            return std::vector<market::Kline>{ k };
        },
        handles, {});

    // Primary market = first handle's symbol.
    EXPECT_EQ("BTC_USDT", ctx.market.ticker.symbol);
    EXPECT_DOUBLE_EQ(65000.0, ctx.market.ticker.last);
    ASSERT_EQ(1u, ctx.market.klines.size());
    EXPECT_DOUBLE_EQ(65001.0, ctx.market.klines[0].close);

    // One-line tickers for both symbols (deduplicated).
    ASSERT_EQ(2u, ctx.symbol_tickers.size());
    EXPECT_DOUBLE_EQ(3500.0, ctx.symbol_tickers[1].last);
}

TEST(PipelineContext, CollectorEmptySourcesDegradeGracefully)
{
    std::vector<StrategyParams> params(1);
    auto handles = make_handles(params);

    const auto ctx = EngineSnapshotCollector::fromSources(
        nullptr, {}, handles, {});

    EXPECT_TRUE(ctx.market.ticker.symbol.empty());
    EXPECT_TRUE(ctx.market.klines.empty());
    EXPECT_TRUE(ctx.symbol_tickers.empty());
}

TEST(PipelineContext, CollectorPassesPerformanceThrough)
{
    TickerCache cache;
    cache.update("BTC_USDT", make_ticker("BTC_USDT", 65000.0));

    std::vector<StrategyParams> params(1);
    auto handles = make_handles(params);
    std::vector<StrategyPerformance> perf{
        { "momentum_scalper_BTC_USDT", "futures", 5, 12.5, 0.8, 0.5 },
    };

    const auto ctx = EngineSnapshotCollector::fromSources(
        &cache, {}, handles, perf);

    ASSERT_EQ(1u, ctx.performance.size());
    EXPECT_EQ("momentum_scalper_BTC_USDT", ctx.performance[0].strategy_name);
    EXPECT_EQ(5, ctx.performance[0].trade_count);
    EXPECT_DOUBLE_EQ(12.5, ctx.performance[0].total_pnl);
    EXPECT_DOUBLE_EQ(0.8, ctx.performance[0].win_rate);
}

TEST(PipelineContext, CollectorDeduplicatesSymbolTickers)
{
    TickerCache cache;
    cache.update("BTC_USDT", make_ticker("BTC_USDT", 65000.0));

    // Two handles on the same symbol → one ticker line.
    std::vector<StrategyParams> params(2);
    std::vector<StrategyHandle> handles{
        { "a", "Scalper", "BTC_USDT", "futures", &params[0] },
        { "b", "Scalper", "BTC_USDT", "futures", &params[1] },
    };

    const auto ctx = EngineSnapshotCollector::fromSources(&cache, {}, handles, {});

    ASSERT_EQ(1u, ctx.symbol_tickers.size());
    EXPECT_EQ("BTC_USDT", ctx.symbol_tickers[0].symbol);
}
