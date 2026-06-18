// test_position_manager.cpp — Unit tests for PositionManager (Layer 7 Risk Management)

#include "risk/position_manager.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::risk;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static RiskConfig make_config(
    double max_notional = 1000.0,
    int max_positions = 5,
    double max_symbol_notional = 500.0)
{
    RiskConfig cfg;
    cfg.maxPositionNotional = max_notional;
    cfg.maxOpenPositions = max_positions;
    cfg.maxSymbolNotional = max_symbol_notional;
    return cfg;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST(PositionManager, DefaultStateHasNoPositions)
{
    PositionManager pm(make_config());
    EXPECT_EQ(0, pm.open_position_count());
    EXPECT_TRUE(pm.get_all_positions().empty());
}

TEST(PositionManager, PortfolioSummaryEmptyOnDefault)
{
    PositionManager pm(make_config());
    const auto summary = pm.portfolio_summary();
    EXPECT_EQ(0, summary.open_position_count);
    EXPECT_DOUBLE_EQ(0.0, summary.total_notional);
    EXPECT_DOUBLE_EQ(0.0, summary.total_unrealized_pnl);
    EXPECT_DOUBLE_EQ(0.0, summary.net_exposure);
}

// ---------------------------------------------------------------------------
// Open position
// ---------------------------------------------------------------------------

TEST(PositionManager, OpenPositionReturnsId)
{
    PositionManager pm(make_config());
    auto result = pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "strategy_1");
    ASSERT_TRUE(ok(result));
    EXPECT_FALSE(value(result).empty());
}

TEST(PositionManager, OpenPositionSetsCorrectFields)
{
    PositionManager pm(make_config());
    auto result = pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "strategy_1");
    ASSERT_TRUE(ok(result));

    const auto pos = pm.get_position(value(result));
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ("BTC_USDT", pos->symbol);
    EXPECT_EQ(Side::Buy, pos->side);
    EXPECT_DOUBLE_EQ(0.01, pos->quantity);
    EXPECT_DOUBLE_EQ(50000.0, pos->entry_price);
    EXPECT_DOUBLE_EQ(50000.0, pos->current_price); // Initially marked at entry.
    EXPECT_DOUBLE_EQ(0.0, pos->unrealized_pnl);
    EXPECT_DOUBLE_EQ(500.0, pos->notional_value); // 0.01 * 50000
    EXPECT_EQ("strategy_1", pos->strategy_id);
}

TEST(PositionManager, OpenPositionIncrementsCount)
{
    PositionManager pm(make_config(10000.0, 5, 10000.0));
    EXPECT_TRUE(ok(pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1")));
    EXPECT_EQ(1, pm.open_position_count());

    EXPECT_TRUE(ok(pm.open_position("ETH_USDT", Side::Sell, 1.0, 3000.0, "s1")));
    EXPECT_EQ(2, pm.open_position_count());
}

// ---------------------------------------------------------------------------
// Open limits
// ---------------------------------------------------------------------------

TEST(PositionManager, RejectsWhenMaxNotionalExceeded)
{
    PositionManager pm(make_config(/*max_notional=*/600.0, 5, 500.0));
    auto r1 = pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r1)); // 500 USDT notional.

    // Second position would add 300 USDT, total 800 > 600.
    auto r2 = pm.open_position("ETH_USDT", Side::Buy, 0.1, 3000.0, "s1");
    ASSERT_FALSE(ok(r2));
    EXPECT_EQ(ErrorCode::PositionLimitHit, error(r2).code);
}

TEST(PositionManager, RejectsWhenMaxPositionsReached)
{
    PositionManager pm(make_config(10000.0, /*max_positions=*/2, 10000.0));

    EXPECT_TRUE(ok(pm.open_position("BTC_USDT", Side::Buy, 0.001, 50000.0, "s1")));
    EXPECT_TRUE(ok(pm.open_position("ETH_USDT", Side::Buy, 0.01, 3000.0, "s1")));

    auto r3 = pm.open_position("SOL_USDT", Side::Buy, 1.0, 100.0, "s1");
    ASSERT_FALSE(ok(r3));
    EXPECT_EQ(ErrorCode::PositionLimitHit, error(r3).code);
}

TEST(PositionManager, RejectsWhenSymbolNotionalExceeded)
{
    PositionManager pm(make_config(10000.0, 10, /*max_symbol_notional=*/500.0));

    auto r1 = pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r1)); // 500 USDT for BTC_USDT.

    // Second BTC_USDT position would exceed per-symbol limit.
    auto r2 = pm.open_position("BTC_USDT", Side::Sell, 0.001, 50000.0, "s1");
    ASSERT_FALSE(ok(r2));
    EXPECT_EQ(ErrorCode::SymbolLimitHit, error(r2).code);
}

TEST(PositionManager, CanOpenPositionPreCheck)
{
    PositionManager pm(make_config(1000.0, 2, 500.0));
    EXPECT_TRUE(pm.can_open_position("BTC_USDT", 0.01, 50000.0)); // 500 <= 500 symbol limit.
    EXPECT_FALSE(pm.can_open_position("BTC_USDT", 0.02, 50000.0)); // 1000 > 500 symbol limit.
}

// ---------------------------------------------------------------------------
// Close position
// ---------------------------------------------------------------------------

TEST(PositionManager, FullCloseRemovesPosition)
{
    PositionManager pm(make_config());
    auto r = pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r));

    EXPECT_TRUE(pm.close_position(value(r), 0.01, 51000.0));
    EXPECT_EQ(0, pm.open_position_count());
    EXPECT_FALSE(pm.get_position(value(r)).has_value());
}

TEST(PositionManager, PartialCloseReducesQuantity)
{
    PositionManager pm(make_config());
    auto r = pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r));

    EXPECT_TRUE(pm.close_position(value(r), 0.005, 51000.0));
    EXPECT_EQ(1, pm.open_position_count());

    const auto pos = pm.get_position(value(r));
    ASSERT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(0.005, pos->quantity);
    EXPECT_DOUBLE_EQ(51000.0, pos->current_price);
}

TEST(PositionManager, CloseNonExistentReturnsFalse)
{
    PositionManager pm(make_config());
    EXPECT_FALSE(pm.close_position("nonexistent", 0.01, 50000.0));
}

TEST(PositionManager, PartialCloseRecalculatesPnl)
{
    PositionManager pm(make_config());
    auto r = pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r));

    // Close half at 52000 — remaining 0.005 BTC, unrealized PnL = (52000-50000)*0.005 = 10.
    EXPECT_TRUE(pm.close_position(value(r), 0.005, 52000.0));

    const auto pos = pm.get_position(value(r));
    ASSERT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(0.005, pos->quantity);
    EXPECT_DOUBLE_EQ(10.0, pos->unrealized_pnl);
    EXPECT_DOUBLE_EQ(260.0, pos->notional_value); // 0.005 * 52000
}

// ---------------------------------------------------------------------------
// Price update
// ---------------------------------------------------------------------------

TEST(PositionManager, UpdatePriceRecalculatesBuyPnl)
{
    PositionManager pm(make_config());
    auto r = pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r));

    pm.update_price(value(r), 52000.0);

    const auto pos = pm.get_position(value(r));
    ASSERT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(52000.0, pos->current_price);
    EXPECT_DOUBLE_EQ(20.0, pos->unrealized_pnl); // (52000-50000)*0.01
    EXPECT_DOUBLE_EQ(520.0, pos->notional_value); // 0.01 * 52000
}

TEST(PositionManager, UpdatePriceRecalculatesSellPnl)
{
    PositionManager pm(make_config(10000.0, 5, 10000.0));
    auto r = pm.open_position("ETH_USDT", Side::Sell, 1.0, 3000.0, "s1");
    ASSERT_TRUE(ok(r));

    // Price drops to 2900 — sell position profits: (3000-2900)*1 = 100.
    pm.update_price(value(r), 2900.0);

    const auto pos = pm.get_position(value(r));
    ASSERT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(2900.0, pos->current_price);
    EXPECT_DOUBLE_EQ(100.0, pos->unrealized_pnl);
    EXPECT_DOUBLE_EQ(2900.0, pos->notional_value);
}

TEST(PositionManager, UpdatePriceNonExistentIsNoOp)
{
    PositionManager pm(make_config());
    pm.update_price("nonexistent", 50000.0); // Should not crash.
    EXPECT_EQ(0, pm.open_position_count());
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

TEST(PositionManager, GetPositionReturnsNulloptForMissing)
{
    PositionManager pm(make_config());
    EXPECT_FALSE(pm.get_position("missing").has_value());
}

TEST(PositionManager, GetPositionsBySymbolFiltersCorrectly)
{
    PositionManager pm(make_config(10000.0, 10, 10000.0));

    EXPECT_TRUE(ok(pm.open_position("BTC_USDT", Side::Buy, 0.001, 50000.0, "s1")));
    EXPECT_TRUE(ok(pm.open_position("ETH_USDT", Side::Buy, 0.1, 3000.0, "s1")));
    EXPECT_TRUE(ok(pm.open_position("BTC_USDT", Side::Sell, 0.001, 50000.0, "s1")));

    const auto btc = pm.get_positions_by_symbol("BTC_USDT");
    EXPECT_EQ(2u, btc.size());

    const auto eth = pm.get_positions_by_symbol("ETH_USDT");
    EXPECT_EQ(1u, eth.size());

    const auto sol = pm.get_positions_by_symbol("SOL_USDT");
    EXPECT_TRUE(sol.empty());
}

TEST(PositionManager, GetPositionsByStrategyFiltersCorrectly)
{
    PositionManager pm(make_config(10000.0, 10, 10000.0));

    EXPECT_TRUE(ok(pm.open_position("BTC_USDT", Side::Buy, 0.001, 50000.0, "scalper")));
    EXPECT_TRUE(ok(pm.open_position("ETH_USDT", Side::Buy, 0.1, 3000.0, "swing")));

    const auto scalper = pm.get_positions_by_strategy("scalper");
    EXPECT_EQ(1u, scalper.size());
    EXPECT_EQ("BTC_USDT", scalper[0].symbol);
}

// ---------------------------------------------------------------------------
// Portfolio summary
// ---------------------------------------------------------------------------

TEST(PositionManager, PortfolioSummaryAggregatesCorrectly)
{
    PositionManager pm(make_config(10000.0, 10, 10000.0));

    EXPECT_TRUE(ok(pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1")));  // notional: 500
    EXPECT_TRUE(ok(pm.open_position("ETH_USDT", Side::Sell, 1.0, 3000.0, "s1")));  // notional: 3000

    // Update BTC price to 52000 — unrealized PnL = (52000-50000)*0.01 = 20.
    auto btc_positions = pm.get_positions_by_symbol("BTC_USDT");
    pm.update_price(btc_positions[0].position_id, 52000.0);

    const auto summary = pm.portfolio_summary();
    EXPECT_EQ(2, summary.open_position_count);
    EXPECT_DOUBLE_EQ(520.0 + 3000.0, summary.total_notional);  // 520 + 3000
    EXPECT_DOUBLE_EQ(20.0, summary.total_unrealized_pnl);       // (52000-50000)*0.01 = 20
    EXPECT_DOUBLE_EQ(520.0 - 3000.0, summary.net_exposure);     // Long - Short.
}

TEST(PositionManager, SymbolNotionalCalculatesCorrectly)
{
    PositionManager pm(make_config(10000.0, 10, 10000.0));

    EXPECT_TRUE(ok(pm.open_position("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1")));  // 500
    EXPECT_TRUE(ok(pm.open_position("ETH_USDT", Side::Buy, 1.0, 3000.0, "s1")));   // 3000

    EXPECT_DOUBLE_EQ(500.0, pm.symbol_notional("BTC_USDT"));
    EXPECT_DOUBLE_EQ(3000.0, pm.symbol_notional("ETH_USDT"));
    EXPECT_DOUBLE_EQ(0.0, pm.symbol_notional("SOL_USDT"));
}

// ---------------------------------------------------------------------------
// Thread safety
// ---------------------------------------------------------------------------

TEST(PositionManager, ConcurrentOpensAreSerialized)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    constexpr int kThreadCount = 4;
    constexpr int kOpensPerThread = 5;
    std::vector<std::jthread> threads;

    for (int t = 0; t < kThreadCount; ++t)
    {
        threads.emplace_back([&pm, t]()
        {
            for (int i = 0; i < kOpensPerThread; ++i)
            {
                (void)pm.open_position("BTC_USDT", Side::Buy, 0.001, 50000.0,
                    "thread_" + std::to_string(t));
            }
        });
    }

    threads.clear(); // Join all threads.
    EXPECT_EQ(kThreadCount * kOpensPerThread, pm.open_position_count());
}

TEST(PositionManager, ConcurrentReadsAndWritesAreSafe)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    // Pre-open some positions.
    for (int i = 0; i < 10; ++i)
    {
        (void)pm.open_position("BTC_USDT", Side::Buy, 0.001, 50000.0, "s1");
    }

    std::atomic<bool> stop{ false };

    // Reader thread: continuously reads portfolio summary.
    std::jthread reader([&pm, &stop]()
    {
        while (!stop.load())
        {
            const auto summary = pm.portfolio_summary();
            (void)summary; // Just exercise the read path.
        }
    });

    // Writer thread: updates prices.
    std::jthread writer([&pm, &stop]()
    {
        double price = 50000.0;
        while (!stop.load())
        {
            const auto positions = pm.get_all_positions();
            for (const auto &pos : positions)
            {
                pm.update_price(pos.position_id, price);
            }
            price += 100.0;
        }
    });

    // Let them run for a brief period.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
}
