// test_stop_loss_engine.cpp — Unit tests for StopLossEngine (Layer 7 Risk Management)

#include "risk/StopLossEngine.hpp"

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
    pos.open_time = now();
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

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(StopLossEngine, DefaultConfigEmptyTracker)
{
    StopLossConfig cfg;
    StopLossEngine engine(cfg);
    EXPECT_EQ(0u, engine.trackedCount());
}

TEST(StopLossEngine, EvaluateEmptyTrackerReturnsNothing)
{
    StopLossEngine engine(StopLossConfig{});
    const auto triggered = engine.evaluate({}, now());
    EXPECT_TRUE(triggered.empty());
}

// ---------------------------------------------------------------------------
// Register / Remove
// ---------------------------------------------------------------------------

TEST(StopLossEngine, RegisterAddsPosition)
{
    StopLossEngine engine(StopLossConfig{});
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);

    EXPECT_TRUE(engine.isTracked("p1"));
    EXPECT_EQ(1u, engine.trackedCount());
}

TEST(StopLossEngine, RemoveErasesPosition)
{
    StopLossEngine engine(StopLossConfig{});
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos);
    engine.removePosition("p1");

    EXPECT_FALSE(engine.isTracked("p1"));
    EXPECT_EQ(0u, engine.trackedCount());
}

TEST(StopLossEngine, RegisterWithCustomConfig)
{
    StopLossConfig custom;
    custom.mode = StopMode::Fixed;
    custom.fixed_pct = 0.05; // 5% stop.

    StopLossEngine engine(StopLossConfig{});
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, custom);

    // 5% below 50000 = 47500. Price at 47400 should trigger.
    auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, 47400.0));
    const auto triggered = engine.evaluate(positions, now());
    EXPECT_EQ(1u, triggered.size());
}

// ---------------------------------------------------------------------------
// Fixed stop (Buy)
// ---------------------------------------------------------------------------

TEST(StopLossEngine, FixedStopBuyTriggersBelowThreshold)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Fixed;
    cfg.fixed_pct = 0.02; // 2% stop.

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // 2% below 50000 = 49000. Price at 48900 should trigger.
    auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, 48900.0));
    const auto triggered = engine.evaluate(positions, now());
    EXPECT_EQ(1u, triggered.size());
    EXPECT_EQ("p1", triggered[0]);
}

TEST(StopLossEngine, FixedStopBuyDoesNotTriggerAboveThreshold)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Fixed;
    cfg.fixed_pct = 0.02;

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // Price at 49500 is above 49000 stop level.
    auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, 49500.0));
    const auto triggered = engine.evaluate(positions, now());
    EXPECT_TRUE(triggered.empty());
}

// ---------------------------------------------------------------------------
// Fixed stop (Sell)
// ---------------------------------------------------------------------------

TEST(StopLossEngine, FixedStopSellTriggersAboveThreshold)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Fixed;
    cfg.fixed_pct = 0.02;

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Sell, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // 2% above 50000 = 51000. Price at 51100 should trigger.
    auto positions = make_positions(make_position("p1", Side::Sell, 50000.0, 51100.0));
    const auto triggered = engine.evaluate(positions, now());
    EXPECT_EQ(1u, triggered.size());
}

TEST(StopLossEngine, FixedStopSellDoesNotTriggerBelowThreshold)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Fixed;
    cfg.fixed_pct = 0.02;

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Sell, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // Price at 50500 is below 51000 stop level.
    auto positions = make_positions(make_position("p1", Side::Sell, 50000.0, 50500.0));
    const auto triggered = engine.evaluate(positions, now());
    EXPECT_TRUE(triggered.empty());
}

// ---------------------------------------------------------------------------
// Trailing stop
// ---------------------------------------------------------------------------

TEST(StopLossEngine, TrailingStopTracksBestPrice)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Trailing;
    cfg.trailing_pct = 0.01; // 1% trail.

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // Price goes up to 52000 — best_price should update.
    auto positions_up = make_positions(make_position("p1", Side::Buy, 50000.0, 52000.0));
    auto triggered = engine.evaluate(positions_up, now());
    EXPECT_TRUE(triggered.empty()); // No trigger, price still above trail.

    // Price drops to 51400 — trail level = 52000 * 0.99 = 51480. 51400 < 51480 -> trigger.
    auto positions_down = make_positions(make_position("p1", Side::Buy, 50000.0, 51400.0));
    triggered = engine.evaluate(positions_down, now());
    EXPECT_EQ(1u, triggered.size());
}

TEST(StopLossEngine, TrailingStopSellTracksLowestPrice)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Trailing;
    cfg.trailing_pct = 0.01;

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Sell, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // Price drops to 48000 — best_price (lowest) should update.
    auto positions_down = make_positions(make_position("p1", Side::Sell, 50000.0, 48000.0));
    auto triggered = engine.evaluate(positions_down, now());
    EXPECT_TRUE(triggered.empty());

    // Price rises to 48500 — trail = 48000 * 1.01 = 48480. 48500 > 48480 -> trigger.
    auto positions_up = make_positions(make_position("p1", Side::Sell, 50000.0, 48500.0));
    triggered = engine.evaluate(positions_up, now());
    EXPECT_EQ(1u, triggered.size());
}

TEST(StopLossEngine, TrailingStopDoesNotTriggerWhileMovingFavorably)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Trailing;
    cfg.trailing_pct = 0.01;

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // Price keeps going up — never triggers.
    for (double price = 50000.0; price <= 55000.0; price += 500.0)
    {
        auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, price));
        const auto triggered = engine.evaluate(positions, now());
        EXPECT_TRUE(triggered.empty());
    }
}

// ---------------------------------------------------------------------------
// Time-based stop
// ---------------------------------------------------------------------------

TEST(StopLossEngine, TimeBasedStopTriggersAfterMaxHold)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::TimeBased;
    cfg.max_hold_seconds = 60; // 1 minute.

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // Evaluate 30 seconds later — should not trigger.
    auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, 50000.0));
    auto t30 = pos.open_time + std::chrono::seconds(30);
    auto triggered = engine.evaluate(positions, t30);
    EXPECT_TRUE(triggered.empty());

    // Evaluate 61 seconds later — should trigger.
    auto t61 = pos.open_time + std::chrono::seconds(61);
    triggered = engine.evaluate(positions, t61);
    EXPECT_EQ(1u, triggered.size());
}

TEST(StopLossEngine, TimeBasedStopExactBoundary)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::TimeBased;
    cfg.max_hold_seconds = 120;

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    auto positions = make_positions(make_position("p1", Side::Buy, 50000.0, 50000.0));
    auto t120 = pos.open_time + std::chrono::seconds(120);
    const auto triggered = engine.evaluate(positions, t120);
    EXPECT_EQ(1u, triggered.size()); // >= max_hold triggers.
}

// ---------------------------------------------------------------------------
// Multiple positions
// ---------------------------------------------------------------------------

TEST(StopLossEngine, EvaluatesMultiplePositions)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Fixed;
    cfg.fixed_pct = 0.02;

    StopLossEngine engine(cfg);

    const auto p1 = make_position("p1", Side::Buy, 50000.0, 50000.0);
    const auto p2 = make_position("p2", Side::Buy, 3000.0, 3000.0);
    engine.registerPosition("p1", p1, cfg);
    engine.registerPosition("p2", p2, cfg);

    // p1 drops below stop (48900 < 49000), p2 stays above (2950 > 2940).
    auto positions = make_positions({
        make_position("p1", Side::Buy, 50000.0, 48900.0),
        make_position("p2", Side::Buy, 3000.0, 2950.0)});
    const auto triggered = engine.evaluate(positions, now());
    EXPECT_EQ(1u, triggered.size());
    EXPECT_EQ("p1", triggered[0]);
}

TEST(StopLossEngine, SkipsClosedPositions)
{
    StopLossConfig cfg;
    cfg.mode = StopMode::Fixed;
    cfg.fixed_pct = 0.02;

    StopLossEngine engine(cfg);
    const auto pos = make_position("p1", Side::Buy, 50000.0, 50000.0);
    engine.registerPosition("p1", pos, cfg);

    // Evaluate with empty positions map — p1 is not found, should be skipped.
    std::unordered_map<std::string, Position> empty_positions;
    const auto triggered = engine.evaluate(empty_positions, now());
    EXPECT_TRUE(triggered.empty());
}
