// test_signal_board.cpp — Unit tests for the signal board (factor board)

#include "strategy/signal/SignalBoard.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace pulse;
using namespace pulse::strategy;

namespace
{

TradingSignal makeSignal(const std::string &id, SignalType type, double conf)
{
    TradingSignal s;
    s.type = type;
    s.symbol = "XAUUSD";
    s.confidence = conf;
    s.price = 4400.0;
    s.strategy_id = id;
    s.reason = "unit test";
    s.market_type = MarketType::Cfd;
    return s;
}

} // anonymous namespace

TEST(SignalBoard, EmptySnapshot)
{
    SignalBoard board(0.6);
    const auto snap = board.snapshot();
    EXPECT_TRUE(snap["signals"].is_array());
    EXPECT_EQ(0u, snap["signals"].size());
    EXPECT_TRUE(snap["aggregate"].is_null());
    EXPECT_EQ(0u, board.entryCount());
}

TEST(SignalBoard, PublishOverwritesLatestPerStrategy)
{
    SignalBoard board(0.6);
    board.publish(makeSignal("s1", SignalType::Buy, 0.5));
    board.publish(makeSignal("s1", SignalType::Sell, 0.8));
    board.publish(makeSignal("s2", SignalType::Flat, 0.1));

    EXPECT_EQ(2u, board.entryCount());
    const auto snap = board.snapshot();
    EXPECT_EQ(2u, snap["signals"].size());

    // s1 must hold the latest (Sell, 0.8), not the first (Buy, 0.5).
    for (const auto &e : snap["signals"])
    {
        if ("s1" == e["source"])
        {
            EXPECT_EQ("sell", e["type"].get<std::string>());
            EXPECT_DOUBLE_EQ(0.8, e["confidence"].get<double>());
            EXPECT_EQ("cfd", e["market_type"].get<std::string>());
            EXPECT_GT(e["ts_ms"].get<std::int64_t>(), 0);
        }
    }
}

TEST(SignalBoard, AggregateCarriesThreshold)
{
    SignalBoard board(0.6);
    board.publishAggregate(makeSignal("aggregate", SignalType::Buy, 0.72));

    const auto snap = board.snapshot();
    ASSERT_TRUE(snap["aggregate"].is_object());
    EXPECT_EQ("aggregate", snap["aggregate"]["source"].get<std::string>());
    EXPECT_EQ("buy", snap["aggregate"]["type"].get<std::string>());
    EXPECT_DOUBLE_EQ(0.72, snap["aggregate"]["confidence"].get<double>());
    EXPECT_DOUBLE_EQ(0.6, snap["aggregate"]["threshold"].get<double>());
}

TEST(SignalBoard, IndicatorsSurviveSnapshot)
{
    SignalBoard board(0.6);
    auto sig = makeSignal("s1", SignalType::Sell, 0.4);
    sig.indicators = { { "ema_fast", 4401.5 }, { "ema_slow", 4399.0 } };
    board.publish(sig);

    const auto snap = board.snapshot();
    ASSERT_EQ(1u, snap["signals"].size());
    const auto &ind = snap["signals"][0]["indicators"];
    EXPECT_DOUBLE_EQ(4401.5, ind["ema_fast"].get<double>());
    EXPECT_DOUBLE_EQ(4399.0, ind["ema_slow"].get<double>());
}

TEST(SignalBoard, AnonymousSignalIsIgnored)
{
    SignalBoard board(0.6);
    board.publish(makeSignal("", SignalType::Buy, 0.9));
    EXPECT_EQ(0u, board.entryCount());
}

TEST(SignalBoard, ConcurrentPublishAndSnapshotSmoke)
{
    SignalBoard board(0.6);
    std::atomic<bool> stop{ false };

    std::thread writer([&board, &stop]()
    {
        int i = 0;
        while (!stop.load())
        {
            board.publish(makeSignal(
                "w" + std::to_string(i % 4),
                (0 == i % 2) ? SignalType::Buy : SignalType::Sell,
                0.1 + static_cast<double>(i % 10) / 10.0));
            ++i;
        }
    });

    for (int round = 0; round < 200; ++round)
    {
        const auto snap = board.snapshot();
        EXPECT_TRUE(snap["signals"].is_array());
    }

    stop.store(true);
    writer.join();
    EXPECT_EQ(4u, board.entryCount());
}
