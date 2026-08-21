// test_market_feed_sink.cpp — MarketDataSink dispatch tests (M18)
//
// Exercises the MarketFeed → MarketDataSink plumbing via the public
// onTickerUpdate/onKlineUpdate seams (no live WebSocket needed).

#include "market/MarketDataSink.hpp"
#include "market/MarketFeed.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::market;

namespace
{

// ---------------------------------------------------------------------------
// RecordingSink — collects every event the feed dispatches.
// ---------------------------------------------------------------------------
class RecordingSink : public MarketDataSink
{
  public:
    void onTicker(const Symbol &symbol, MarketType market_type,
                  const Ticker &ticker) override
    {
        tickers.push_back({ symbol, market_type, ticker });
    }

    void onKline(const Symbol &symbol, MarketType market_type,
                 const Kline &kline) override
    {
        klines.push_back({ symbol, market_type, kline });
    }

    struct TickerEvent
    {
        std::string symbol;
        MarketType market_type;
        Ticker ticker;
    };
    struct KlineEvent
    {
        std::string symbol;
        MarketType market_type;
        Kline kline;
    };

    std::vector<TickerEvent> tickers;
    std::vector<KlineEvent> klines;
};

// ---------------------------------------------------------------------------
// Helpers — feed construction (no network I/O in ctor; start() is never
// called here, so the REST client is never actually used).
// ---------------------------------------------------------------------------

struct FeedHarness
{
    exchange::GateRestClient rest;
    MarketFeed feed;

    explicit FeedHarness(MarketType market_type)
        : rest{ ExchangeConfig{}, market_type }
        , feed{ nullptr, rest, market_type }
    {
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Ticker dispatch
// ---------------------------------------------------------------------------

TEST(MarketFeedSink, SpotTickerDispatchedToSink)
{
    FeedHarness harness{ MarketType::Spot };
    auto &feed = harness.feed;
    RecordingSink sink;
    feed.setMarketDataSink(&sink);

    // Spot ticker: result is an OBJECT with "currency_pair".
    const nlohmann::json frame = nlohmann::json::parse(R"({
        "time": 1542162490,
        "channel": "spot.tickers",
        "event": "update",
        "result": {
            "currency_pair": "BTC_USDT",
            "last": "50000.5",
            "highest_bid": "49999.0",
            "lowest_ask": "50001.0",
            "base_volume": "1234.56",
            "change_percentage": "0.58"
        }
    })");

    feed.onTickerUpdate(frame["result"], frame);

    ASSERT_EQ(1u, sink.tickers.size());
    const auto &ev = sink.tickers[0];
    EXPECT_EQ("BTC_USDT", ev.symbol);
    EXPECT_EQ(MarketType::Spot, ev.market_type);
    EXPECT_DOUBLE_EQ(50000.5, ev.ticker.last);
    EXPECT_DOUBLE_EQ(49999.0, ev.ticker.bid);
    EXPECT_DOUBLE_EQ(50001.0, ev.ticker.ask);
    EXPECT_DOUBLE_EQ(1234.56, ev.ticker.volume_24h);
    EXPECT_GT(ev.ticker.timestamp, 0); // Stamped with nowMs().
    EXPECT_EQ(0u, sink.klines.size());
}

TEST(MarketFeedSink, FuturesTickerArrayDispatched)
{
    FeedHarness harness{ MarketType::Futures };
    auto &feed = harness.feed;
    RecordingSink sink;
    feed.setMarketDataSink(&sink);

    // Futures ticker: result is an ARRAY with one element carrying "contract".
    const nlohmann::json frame = nlohmann::json::parse(R"({
        "time": 1542162490,
        "channel": "futures.tickers",
        "event": "update",
        "result": [{
            "contract": "BTC_USDT",
            "last": "50000.5",
            "mark_price": "50001.2",
            "index_price": "50000.9",
            "funding_rate": "0.0001",
            "volume_24h": "1000",
            "change_percentage": "-0.42"
        }]
    })");

    feed.onTickerUpdate(frame["result"], frame);

    ASSERT_EQ(1u, sink.tickers.size());
    const auto &ev = sink.tickers[0];
    EXPECT_EQ("BTC_USDT", ev.symbol);
    EXPECT_EQ(MarketType::Futures, ev.market_type);
    EXPECT_DOUBLE_EQ(50000.5, ev.ticker.last);
    EXPECT_DOUBLE_EQ(50001.2, ev.ticker.mark_price);
    EXPECT_DOUBLE_EQ(50000.9, ev.ticker.index_price);
    EXPECT_DOUBLE_EQ(0.0001, ev.ticker.funding_rate);
    EXPECT_GT(ev.ticker.timestamp, 0);
}

// ---------------------------------------------------------------------------
// Kline dispatch
// ---------------------------------------------------------------------------

TEST(MarketFeedSink, SpotKlineDispatchedToSink)
{
    FeedHarness harness{ MarketType::Spot };
    auto &feed = harness.feed;
    RecordingSink sink;
    feed.setMarketDataSink(&sink);

    // Spot kline: symbol in the outer frame as "currency_pair".
    const nlohmann::json frame = nlohmann::json::parse(R"({
        "time": 1542162490,
        "channel": "spot.candlesticks",
        "event": "update",
        "result": { "t": 1542162480, "o": "6350.1", "c": "6350.2",
                    "h": "6350.2", "l": "6350.1", "v": "120", "n": "1m" },
        "currency_pair": "BTC_USDT"
    })");

    feed.onKlineUpdate(frame["result"], frame);

    ASSERT_EQ(1u, sink.klines.size());
    const auto &ev = sink.klines[0];
    EXPECT_EQ("BTC_USDT", ev.symbol);
    EXPECT_EQ(MarketType::Spot, ev.market_type);
    EXPECT_DOUBLE_EQ(6350.1, ev.kline.open);
    EXPECT_DOUBLE_EQ(6350.2, ev.kline.high);
    EXPECT_DOUBLE_EQ(6350.1, ev.kline.low);
    EXPECT_DOUBLE_EQ(6350.2, ev.kline.close);
    EXPECT_DOUBLE_EQ(120.0, ev.kline.volume);
    EXPECT_EQ(0u, sink.tickers.size());
}

TEST(MarketFeedSink, FuturesKlineRoutesToPerSymbolBuffer)
{
    // Futures candlestick pushes carry the contract in "n" as
    // "<interval>_<contract>" (see FuturesKlineFrameCarriesSymbolInInterval
    // Field) — each pushed candle must land in ITS OWN buffer. Regression
    // for the 08-21 bug where every futures kline fell into the first
    // subscribed symbol's buffer.
    FeedHarness harness{ MarketType::Futures };
    auto &feed = harness.feed;

    const nlohmann::json eth_frame = nlohmann::json::parse(R"({
        "time": 1542162490,
        "channel": "futures.candlesticks",
        "event": "update",
        "result": [ { "t": 1542162480, "o": "6350.1", "c": "6350.2",
                      "h": "6350.2", "l": "6350.1", "v": 120,
                      "n": "1m_ETH_USDT", "w": false } ]
    })");
    const nlohmann::json sndk_frame = nlohmann::json::parse(R"({
        "time": 1542162490,
        "channel": "futures.candlesticks",
        "event": "update",
        "result": [ { "t": 1542162480, "o": "98.1", "c": "98.9",
                      "h": "99.1", "l": "98.0", "v": 120,
                      "n": "1m_SNDK_USDT", "w": false } ]
    })");

    // No fallback symbol passed — the "n" field must route each candle.
    feed.onKlineUpdate(eth_frame["result"], eth_frame);
    feed.onKlineUpdate(sndk_frame["result"], sndk_frame);

    const auto eth = feed.getKlineBuffer("ETH_USDT").snapshot(1);
    const auto sndk = feed.getKlineBuffer("SNDK_USDT").snapshot(1);
    ASSERT_EQ(1u, eth.size());
    ASSERT_EQ(1u, sndk.size());
    EXPECT_DOUBLE_EQ(6350.2, eth[0].close);
    EXPECT_DOUBLE_EQ(98.9, sndk[0].close);

    // No stray candle in the other buffers.
    EXPECT_TRUE(feed.getKlineBuffer("BTC_USDT").snapshot(1).empty());
}

// ---------------------------------------------------------------------------
// Null sink
// ---------------------------------------------------------------------------

TEST(MarketFeedSink, NullSinkIsSafe)
{
    // Default feed has no sink — dispatch must be a silent no-op.
    FeedHarness harness{ MarketType::Spot };
    auto &feed = harness.feed;

    const nlohmann::json ticker_frame = nlohmann::json::parse(R"({
        "result": { "currency_pair": "BTC_USDT", "last": "50000.5",
                    "highest_bid": "49999.0", "lowest_ask": "50001.0",
                    "base_volume": "1234.56", "change_percentage": "0.58" }
    })");
    const nlohmann::json kline_frame = nlohmann::json::parse(R"({
        "result": { "t": 1542162480, "o": "6350.1", "c": "6350.2",
                    "h": "6350.2", "l": "6350.1", "v": "120", "n": "1m" },
        "currency_pair": "BTC_USDT"
    })");

    EXPECT_NO_THROW(feed.onTickerUpdate(ticker_frame["result"], ticker_frame));
    EXPECT_NO_THROW(feed.onKlineUpdate(kline_frame["result"], kline_frame));

    // Counter stats still advance (dispatch is decoupled from caching).
    const auto stats = feed.stats();
    EXPECT_EQ(1u, stats.ticker_count);
    EXPECT_EQ(1u, stats.kline_count);
}
