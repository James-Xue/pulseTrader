// test_protective_exit.cpp — Unit tests for ProtectiveExitManager (Layer 7)

#include "risk/ProtectiveExitManager.hpp"

#include "risk/PositionManager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <vector>

using namespace pulse;
using namespace pulse::risk;

namespace
{

RiskConfig make_config()
{
    RiskConfig cfg;
    cfg.maxPositionNotional = 1e9;
    cfg.maxPositionNotionalFutures = 1e9;
    cfg.maxPositionNotionalCfd = 1e9;
    cfg.maxPositionNotionalSpot = 1e9;
    cfg.maxSymbolNotional = 1e9;
    cfg.maxOpenPositions = 20;
    return cfg;
}

Timestamp at(std::int64_t seconds)
{
    return Timestamp{ std::chrono::seconds{ seconds } };
}

} // namespace

// ---------------------------------------------------------------------------
// Stop-loss: fixed distance → full close
// ---------------------------------------------------------------------------

TEST(ProtectiveExit, FixedStopFiresFullClose)
{
    auto cfg = make_config();
    cfg.stop_loss.mode = StopMode::Fixed;
    cfg.stop_loss.fixed_pct = 0.01;
    cfg.take_profit.enabled = false;

    PositionManager positions(cfg);
    auto opened = positions.openPosition("BTC_USDT", Side::Buy, 1.0, 100.0, "s1",
                                         MarketType::Futures, 10.0, MarginMode::Cross,
                                         1.0, 0.005);
    ASSERT_TRUE(ok(opened)) << error(opened).message;

    std::vector<double> closes;
    ProtectiveExitManager exits(
        cfg, positions,
        [](const Symbol &, MarketType) { return std::optional<Price>{ 98.0 }; },
        [&](const Position &, double qty) {
            closes.push_back(qty);
            return true;
        });

    const auto r1 = exits.tick(at(10));
    EXPECT_EQ(1, r1.stopped);
    ASSERT_EQ(1u, closes.size());
    EXPECT_DOUBLE_EQ(1.0, closes[0]);

    // Cooldown: an immediate second tick must not place another close.
    const auto r2 = exits.tick(at(11));
    EXPECT_EQ(0, r2.stopped);
    EXPECT_EQ(1u, closes.size());
}

TEST(ProtectiveExit, FailedCloseRetriesAfterCooldown)
{
    auto cfg = make_config();
    cfg.stop_loss.mode = StopMode::Fixed;
    cfg.stop_loss.fixed_pct = 0.01;
    cfg.take_profit.enabled = false;

    PositionManager positions(cfg);
    auto opened = positions.openPosition("BTC_USDT", Side::Buy, 1.0, 100.0, "s1",
                                         MarketType::Futures, 10.0, MarginMode::Cross,
                                         1.0, 0.005);
    ASSERT_TRUE(ok(opened)) << error(opened).message;

    int attempts = 0;
    ProtectiveExitManager exits(
        cfg, positions,
        [](const Symbol &, MarketType) { return std::optional<Price>{ 98.0 }; },
        [&](const Position &, double) {
            ++attempts;
            return false; // every attempt fails
        });

    EXPECT_EQ(1, exits.tick(at(10)).failed);
    EXPECT_EQ(1, attempts);

    // Inside the cooldown window: no new attempt.
    EXPECT_EQ(0, exits.tick(at(19)).failed);
    EXPECT_EQ(1, attempts);

    // Cooldown elapsed: retry.
    EXPECT_EQ(1, exits.tick(at(20)).failed);
    EXPECT_EQ(2, attempts);
}

TEST(ProtectiveExit, ClosesRegistrationWhenPositionDisappears)
{
    auto cfg = make_config();
    cfg.stop_loss.mode = StopMode::Fixed;
    cfg.stop_loss.fixed_pct = 0.01;
    cfg.take_profit.enabled = false;

    PositionManager positions(cfg);
    auto opened = positions.openPosition("BTC_USDT", Side::Buy, 1.0, 100.0, "s1",
                                         MarketType::Futures, 10.0, MarginMode::Cross,
                                         1.0, 0.005);
    ASSERT_TRUE(ok(opened)) << error(opened).message;
    const std::string id = value(opened);

    ProtectiveExitManager exits(
        cfg, positions,
        [](const Symbol &, MarketType) { return std::optional<Price>{ 100.0 }; },
        [](const Position &, double) { return true; });

    EXPECT_EQ(0, exits.tick(at(10)).stopped); // no stop at entry price
    EXPECT_EQ(1u, exits.trackedPositions());

    // Simulate an external close (manual app close / exchange stop).
    (void)positions.closePosition(id, 1.0, 100.0);

    exits.tick(at(11));
    EXPECT_EQ(0u, exits.trackedPositions());
}

// ---------------------------------------------------------------------------
// Take-profit: ladder target → partial close
// ---------------------------------------------------------------------------

TEST(ProtectiveExit, TakeProfitClosesFraction)
{
    auto cfg = make_config();
    cfg.stop_loss.mode = StopMode::Fixed;
    cfg.stop_loss.fixed_pct = 0.50; // far away — must not fire
    cfg.take_profit.enabled = true;
    cfg.take_profit.targets_pct = { 0.01 };
    cfg.take_profit.fractions = { 0.5 };

    PositionManager positions(cfg);
    auto opened = positions.openPosition("BTC_USDT", Side::Buy, 2.0, 100.0, "s1",
                                         MarketType::Futures, 10.0, MarginMode::Cross,
                                         1.0, 0.005);
    ASSERT_TRUE(ok(opened)) << error(opened).message;

    std::vector<double> closes;
    ProtectiveExitManager exits(
        cfg, positions,
        [](const Symbol &, MarketType) { return std::optional<Price>{ 101.0 }; },
        [&](const Position &, double qty) {
            closes.push_back(qty);
            return true;
        });

    const auto r = exits.tick(at(10));
    EXPECT_EQ(0, r.stopped);
    EXPECT_EQ(1, r.took_profit);
    ASSERT_EQ(1u, closes.size());
    EXPECT_DOUBLE_EQ(1.0, closes[0]); // 50% of the 2.0 position
}

// ---------------------------------------------------------------------------
// Missing quote: no spurious stop
// ---------------------------------------------------------------------------

TEST(ProtectiveExit, MissingQuoteDoesNotTrigger)
{
    auto cfg = make_config();
    cfg.stop_loss.mode = StopMode::Fixed;
    cfg.stop_loss.fixed_pct = 0.01;
    cfg.take_profit.enabled = false;

    PositionManager positions(cfg);
    auto opened = positions.openPosition("BTC_USDT", Side::Buy, 1.0, 100.0, "s1",
                                         MarketType::Futures, 10.0, MarginMode::Cross,
                                         1.0, 0.005);
    ASSERT_TRUE(ok(opened)) << error(opened).message;

    ProtectiveExitManager exits(
        cfg, positions,
        [](const Symbol &, MarketType) { return std::optional<Price>{}; },
        [](const Position &, double) { return true; });

    const auto r = exits.tick(at(10));
    EXPECT_EQ(0, r.stopped);
    EXPECT_EQ(0, r.took_profit);
}
TEST(ProtectiveExit, ManageFilterSkipsExcludedPositions)
{
    auto cfg = make_config();
    cfg.stop_loss.mode = StopMode::Fixed;
    cfg.stop_loss.fixed_pct = 0.01;
    cfg.take_profit.enabled = false;

    PositionManager positions(cfg);
    auto opened = positions.openPosition("XAUUSD", Side::Buy, 0.01, 4000.0,
                                         "gold", MarketType::Cfd, 500.0,
                                         MarginMode::Cross, 100.0, 0.0);
    ASSERT_TRUE(ok(opened)) << error(opened).message;

    int closes = 0;
    ProtectiveExitManager exits(
        cfg, positions,
        [](const Symbol &, MarketType) { return std::optional<Price>{ 3900.0 }; },
        [&](const Position &, double) {
            ++closes;
            return true;
        },
        [](const Position &pos) { return pos.symbol != "XAUUSD"; });

    EXPECT_EQ(0, exits.tick(at(10)).stopped);
    EXPECT_EQ(0, closes);
    EXPECT_EQ(0u, exits.trackedPositions());
}
