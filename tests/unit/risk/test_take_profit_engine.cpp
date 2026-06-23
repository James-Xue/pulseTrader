// test_take_profit_engine.cpp — Unit tests for TakeProfitEngine (Layer 7 Risk Management)

#include "risk/TakeProfitEngine.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::risk;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Position make_position(const std::string &id, Side side, Price entry, Price current)
{
    Position pos;
    pos.position_id = id;
    pos.symbol = "BTC_USDT";
    pos.side = side;
    pos.quantity = 0.01;
    pos.entry_price = entry;
    pos.current_price = current;
    return pos;
}

static std::unordered_map<std::string, Position> make_positions(std::initializer_list<Position> positions)
{
    std::unordered_map<std::string, Position> map;
    for (const auto &pos : positions)
    {
        map[pos.position_id] = pos;
    }
    return map;
}

/// Overload for single position (avoids brace syntax for single elements).
static std::unordered_map<std::string, Position> make_positions(const Position &pos)
{
    return { { pos.position_id, pos } };
}

static TakeProfitConfig make_default_tp_config()
{
    TakeProfitConfig cfg;
    cfg.enabled = true;
    cfg.targets_pct = { 0.01, 0.02, 0.05 }; // 1%, 2%, 5% targets.
    cfg.fractions = { 0.30, 0.30, 0.40 };   // 30%, 30%, 40%.
    return cfg;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(TakeProfitEngine, DefaultConfigEmptyTracker)
{
    TakeProfitEngine engine(make_default_tp_config());
    EXPECT_EQ(0u, engine.trackedCount());
}

TEST(TakeProfitEngine, EvaluateEmptyTrackerReturnsNothing)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto signals = engine.evaluate({});
    EXPECT_TRUE(signals.empty());
}

// ---------------------------------------------------------------------------
// Register / Remove
// ---------------------------------------------------------------------------

TEST(TakeProfitEngine, RegisterAddsPosition)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);

    EXPECT_TRUE(engine.isTracked("p1"));
    EXPECT_EQ(1u, engine.trackedCount());
    EXPECT_EQ(0, engine.nextTargetIndex("p1"));
}

TEST(TakeProfitEngine, RemoveErasesPosition)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);
    engine.removePosition("p1");

    EXPECT_FALSE(engine.isTracked("p1"));
    EXPECT_EQ(-1, engine.nextTargetIndex("p1")); // Not found returns -1.
}

// ---------------------------------------------------------------------------
// Single target (Buy)
// ---------------------------------------------------------------------------

TEST(TakeProfitEngine, BuyTargetTriggersAtPrice)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);

    // 1% above 50000 = 50500. Price at 50600 should trigger.
    auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, 50600.0));
    const auto signals = engine.evaluate(positions);
    ASSERT_EQ(1u, signals.size());
    EXPECT_EQ("p1", signals[0].position_id);
    EXPECT_DOUBLE_EQ(0.30, signals[0].close_fraction);
    EXPECT_EQ(0, signals[0].target_index);
}

TEST(TakeProfitEngine, BuyTargetDoesNotTriggerBelowPrice)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);

    // Price at 50400 is below 50500 target.
    auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, 50400.0));
    const auto signals = engine.evaluate(positions);
    EXPECT_TRUE(signals.empty());
}

// ---------------------------------------------------------------------------
// Sell target
// ---------------------------------------------------------------------------

TEST(TakeProfitEngine, SellTargetTriggersAtPrice)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto pos = make_position("p1", Side::Sell, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);

    // 1% below 50000 = 49500. Price at 49400 should trigger.
    auto positions = make_positions(make_position("p1", Side::Sell, 50000.0, 49400.0));
    const auto signals = engine.evaluate(positions);
    ASSERT_EQ(1u, signals.size());
    EXPECT_DOUBLE_EQ(0.30, signals[0].close_fraction);
}

TEST(TakeProfitEngine, SellTargetDoesNotTriggerAbovePrice)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto pos = make_position("p1", Side::Sell, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);

    // Price at 49600 is above 49500 target.
    auto positions = make_positions(make_position("p1", Side::Sell, 50000.0, 49600.0));
    const auto signals = engine.evaluate(positions);
    EXPECT_TRUE(signals.empty());
}

// ---------------------------------------------------------------------------
// Ladder progression
// ---------------------------------------------------------------------------

TEST(TakeProfitEngine, FirstTargetAdvancesIndex)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);

    // Hit first target (50500).
    auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, 50600.0));
    auto signals = engine.evaluate(positions);
    ASSERT_EQ(1u, signals.size());
    EXPECT_EQ(0, signals[0].target_index);
    EXPECT_EQ(1, engine.nextTargetIndex("p1"));

    // Hit second target (51000).
    positions = make_positions(make_position("p1", Side::Buy, 50000.0, 51100.0));
    signals = engine.evaluate(positions);
    ASSERT_EQ(1u, signals.size());
    EXPECT_EQ(1, signals[0].target_index);
    EXPECT_DOUBLE_EQ(0.30, signals[0].close_fraction);
    EXPECT_EQ(2, engine.nextTargetIndex("p1"));
}

TEST(TakeProfitEngine, AllTargetsConsumedNoMoreSignals)
{
    TakeProfitEngine engine(make_default_tp_config());
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);

    // Hit all 3 targets sequentially.
    auto p1 = make_positions(make_position("p1", Side::Buy, 50000.0, 50600.0));
    (void)engine.evaluate(p1); // Target 0.

    auto p2 = make_positions(make_position("p1", Side::Buy, 50000.0, 51100.0));
    (void)engine.evaluate(p2); // Target 1.

    auto p3 = make_positions(make_position("p1", Side::Buy, 50000.0, 52600.0));
    (void)engine.evaluate(p3); // Target 2.

    EXPECT_EQ(3, engine.nextTargetIndex("p1"));

    // Further evaluations produce no signals.
    auto p4 = make_positions(make_position("p1", Side::Buy, 50000.0, 60000.0));
    const auto signals = engine.evaluate(p4);
    EXPECT_TRUE(signals.empty());
}

// ---------------------------------------------------------------------------
// Multiple positions
// ---------------------------------------------------------------------------

TEST(TakeProfitEngine, IndependentTrackingPerPosition)
{
    TakeProfitEngine engine(make_default_tp_config());

    const auto p1 = make_position("p1", Side::Buy, 50000.0, 50000.0);
    const auto p2 = make_position("p2", Side::Buy, 3000.0, 3000.0);
    engine.registerPosition("p1", p1);
    engine.registerPosition("p2", p2);

    // p1 hits first target (50500), p2 does not (needs 3030).
    auto positions = make_positions({
        make_position("p1", Side::Buy, 50000.0, 50600.0),
        make_position("p2", Side::Buy, 3000.0, 3020.0)});
    const auto signals = engine.evaluate(positions);
    ASSERT_EQ(1u, signals.size());
    EXPECT_EQ("p1", signals[0].position_id);

    EXPECT_EQ(1, engine.nextTargetIndex("p1"));
    EXPECT_EQ(0, engine.nextTargetIndex("p2"));
}

TEST(TakeProfitEngine, MultiplePositionsTriggerSimultaneously)
{
    TakeProfitEngine engine(make_default_tp_config());

    const auto p1 = make_position("p1", Side::Buy, 50000.0, 50000.0);
    const auto p2 = make_position("p2", Side::Buy, 3000.0, 3000.0);
    engine.registerPosition("p1", p1);
    engine.registerPosition("p2", p2);

    // Both hit their first targets.
    auto positions = make_positions({
        make_position("p1", Side::Buy, 50000.0, 50600.0),
        make_position("p2", Side::Buy, 3000.0, 3040.0)});
    const auto signals = engine.evaluate(positions);
    EXPECT_EQ(2u, signals.size());
}
