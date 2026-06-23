// test_position_manager.cpp — Unit tests for PositionManager (Layer 7 Risk Management)

#include "risk/PositionManager.hpp"

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
    EXPECT_EQ(0, pm.openPositionCount());
    EXPECT_TRUE(pm.getAllPositions().empty());
}

TEST(PositionManager, PortfolioSummaryEmptyOnDefault)
{
    PositionManager pm(make_config());
    const auto summary = pm.portfolioSummary();
    EXPECT_EQ(0, summary.openPositionCount);
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
    auto result = pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "strategy_1");
    ASSERT_TRUE(ok(result));
    EXPECT_FALSE(value(result).empty());
}

TEST(PositionManager, OpenPositionSetsCorrectFields)
{
    PositionManager pm(make_config());
    auto result = pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "strategy_1");
    ASSERT_TRUE(ok(result));

    const auto pos = pm.getPosition(value(result));
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
    EXPECT_TRUE(ok(pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1")));
    EXPECT_EQ(1, pm.openPositionCount());

    EXPECT_TRUE(ok(pm.openPosition("ETH_USDT", Side::Sell, 1.0, 3000.0, "s1")));
    EXPECT_EQ(2, pm.openPositionCount());
}

// ---------------------------------------------------------------------------
// Open limits
// ---------------------------------------------------------------------------

TEST(PositionManager, RejectsWhenMaxNotionalExceeded)
{
    PositionManager pm(make_config(/*max_notional=*/600.0, 5, 500.0));
    auto r1 = pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r1)); // 500 USDT notional.

    // Second position would add 300 USDT, total 800 > 600.
    auto r2 = pm.openPosition("ETH_USDT", Side::Buy, 0.1, 3000.0, "s1");
    ASSERT_FALSE(ok(r2));
    EXPECT_EQ(ErrorCode::PositionLimitHit, error(r2).code);
}

TEST(PositionManager, RejectsWhenMaxPositionsReached)
{
    PositionManager pm(make_config(10000.0, /*max_positions=*/2, 10000.0));

    EXPECT_TRUE(ok(pm.openPosition("BTC_USDT", Side::Buy, 0.001, 50000.0, "s1")));
    EXPECT_TRUE(ok(pm.openPosition("ETH_USDT", Side::Buy, 0.01, 3000.0, "s1")));

    auto r3 = pm.openPosition("SOL_USDT", Side::Buy, 1.0, 100.0, "s1");
    ASSERT_FALSE(ok(r3));
    EXPECT_EQ(ErrorCode::PositionLimitHit, error(r3).code);
}

TEST(PositionManager, RejectsWhenSymbolNotionalExceeded)
{
    PositionManager pm(make_config(10000.0, 10, /*max_symbol_notional=*/500.0));

    auto r1 = pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r1)); // 500 USDT for BTC_USDT.

    // Second BTC_USDT position would exceed per-symbol limit.
    auto r2 = pm.openPosition("BTC_USDT", Side::Sell, 0.001, 50000.0, "s1");
    ASSERT_FALSE(ok(r2));
    EXPECT_EQ(ErrorCode::SymbolLimitHit, error(r2).code);
}

TEST(PositionManager, CanOpenPositionPreCheck)
{
    PositionManager pm(make_config(1000.0, 2, 500.0));
    EXPECT_TRUE(pm.canOpenPosition("BTC_USDT", 0.01, 50000.0)); // 500 <= 500 symbol limit.
    EXPECT_FALSE(pm.canOpenPosition("BTC_USDT", 0.02, 50000.0)); // 1000 > 500 symbol limit.
}

// ---------------------------------------------------------------------------
// Close position
// ---------------------------------------------------------------------------

TEST(PositionManager, FullCloseRemovesPosition)
{
    PositionManager pm(make_config());
    auto r = pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r));

    auto pnl = pm.closePosition(value(r), 0.01, 51000.0);
    ASSERT_TRUE(pnl.has_value());
    // Buy 0.01 BTC @ 50000, sell @ 51000 → PnL = (51000-50000)*0.01 = 10.0
    EXPECT_DOUBLE_EQ(10.0, pnl.value());
    EXPECT_EQ(0, pm.openPositionCount());
    EXPECT_FALSE(pm.getPosition(value(r)).has_value());
}

TEST(PositionManager, PartialCloseReducesQuantity)
{
    PositionManager pm(make_config());
    auto r = pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r));

    auto pnl = pm.closePosition(value(r), 0.005, 51000.0);
    ASSERT_TRUE(pnl.has_value());
    // Buy 0.01 @ 50000, close 0.005 @ 51000 → realized = (51000-50000)*0.005 = 5.0
    EXPECT_DOUBLE_EQ(5.0, pnl.value());
    EXPECT_EQ(1, pm.openPositionCount());

    const auto pos = pm.getPosition(value(r));
    ASSERT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(0.005, pos->quantity);
    EXPECT_DOUBLE_EQ(51000.0, pos->current_price);
}

TEST(PositionManager, CloseNonExistentReturnsNullopt)
{
    PositionManager pm(make_config());
    auto result = pm.closePosition("nonexistent", 0.01, 50000.0);
    EXPECT_FALSE(result.has_value());
}

TEST(PositionManager, PartialCloseRecalculatesPnl)
{
    PositionManager pm(make_config());
    auto r = pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r));

    // Close half at 52000 — remaining 0.005 BTC, unrealized PnL = (52000-50000)*0.005 = 10.
    auto pnl = pm.closePosition(value(r), 0.005, 52000.0);
    ASSERT_TRUE(pnl.has_value());
    // Realized PnL for closed half: (52000-50000)*0.005 = 10.0
    EXPECT_DOUBLE_EQ(10.0, pnl.value());

    const auto pos = pm.getPosition(value(r));
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
    auto r = pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1");
    ASSERT_TRUE(ok(r));

    pm.updatePrice(value(r), 52000.0);

    const auto pos = pm.getPosition(value(r));
    ASSERT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(52000.0, pos->current_price);
    EXPECT_DOUBLE_EQ(20.0, pos->unrealized_pnl); // (52000-50000)*0.01
    EXPECT_DOUBLE_EQ(520.0, pos->notional_value); // 0.01 * 52000
}

TEST(PositionManager, UpdatePriceRecalculatesSellPnl)
{
    PositionManager pm(make_config(10000.0, 5, 10000.0));
    auto r = pm.openPosition("ETH_USDT", Side::Sell, 1.0, 3000.0, "s1");
    ASSERT_TRUE(ok(r));

    // Price drops to 2900 — sell position profits: (3000-2900)*1 = 100.
    pm.updatePrice(value(r), 2900.0);

    const auto pos = pm.getPosition(value(r));
    ASSERT_TRUE(pos.has_value());
    EXPECT_DOUBLE_EQ(2900.0, pos->current_price);
    EXPECT_DOUBLE_EQ(100.0, pos->unrealized_pnl);
    EXPECT_DOUBLE_EQ(2900.0, pos->notional_value);
}

TEST(PositionManager, UpdatePriceNonExistentIsNoOp)
{
    PositionManager pm(make_config());
    pm.updatePrice("nonexistent", 50000.0); // Should not crash.
    EXPECT_EQ(0, pm.openPositionCount());
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

TEST(PositionManager, GetPositionReturnsNulloptForMissing)
{
    PositionManager pm(make_config());
    EXPECT_FALSE(pm.getPosition("missing").has_value());
}

TEST(PositionManager, GetPositionsBySymbolFiltersCorrectly)
{
    PositionManager pm(make_config(10000.0, 10, 10000.0));

    EXPECT_TRUE(ok(pm.openPosition("BTC_USDT", Side::Buy, 0.001, 50000.0, "s1")));
    EXPECT_TRUE(ok(pm.openPosition("ETH_USDT", Side::Buy, 0.1, 3000.0, "s1")));
    EXPECT_TRUE(ok(pm.openPosition("BTC_USDT", Side::Sell, 0.001, 50000.0, "s1")));

    const auto btc = pm.getPositionsBySymbol("BTC_USDT");
    EXPECT_EQ(2u, btc.size());

    const auto eth = pm.getPositionsBySymbol("ETH_USDT");
    EXPECT_EQ(1u, eth.size());

    const auto sol = pm.getPositionsBySymbol("SOL_USDT");
    EXPECT_TRUE(sol.empty());
}

TEST(PositionManager, GetPositionsByStrategyFiltersCorrectly)
{
    PositionManager pm(make_config(10000.0, 10, 10000.0));

    EXPECT_TRUE(ok(pm.openPosition("BTC_USDT", Side::Buy, 0.001, 50000.0, "scalper")));
    EXPECT_TRUE(ok(pm.openPosition("ETH_USDT", Side::Buy, 0.1, 3000.0, "swing")));

    const auto scalper = pm.getPositionsByStrategy("scalper");
    EXPECT_EQ(1u, scalper.size());
    EXPECT_EQ("BTC_USDT", scalper[0].symbol);
}

// ---------------------------------------------------------------------------
// Portfolio summary
// ---------------------------------------------------------------------------

TEST(PositionManager, PortfolioSummaryAggregatesCorrectly)
{
    PositionManager pm(make_config(10000.0, 10, 10000.0));

    EXPECT_TRUE(ok(pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1")));  // notional: 500
    EXPECT_TRUE(ok(pm.openPosition("ETH_USDT", Side::Sell, 1.0, 3000.0, "s1")));  // notional: 3000

    // Update BTC price to 52000 — unrealized PnL = (52000-50000)*0.01 = 20.
    auto btc_positions = pm.getPositionsBySymbol("BTC_USDT");
    pm.updatePrice(btc_positions[0].position_id, 52000.0);

    const auto summary = pm.portfolioSummary();
    EXPECT_EQ(2, summary.openPositionCount);
    EXPECT_DOUBLE_EQ(520.0 + 3000.0, summary.total_notional);  // 520 + 3000
    EXPECT_DOUBLE_EQ(20.0, summary.total_unrealized_pnl);       // (52000-50000)*0.01 = 20
    EXPECT_DOUBLE_EQ(520.0 - 3000.0, summary.net_exposure);     // Long - Short.
}

TEST(PositionManager, SymbolNotionalCalculatesCorrectly)
{
    PositionManager pm(make_config(10000.0, 10, 10000.0));

    EXPECT_TRUE(ok(pm.openPosition("BTC_USDT", Side::Buy, 0.01, 50000.0, "s1")));  // 500
    EXPECT_TRUE(ok(pm.openPosition("ETH_USDT", Side::Buy, 1.0, 3000.0, "s1")));   // 3000

    EXPECT_DOUBLE_EQ(500.0, pm.symbolNotional("BTC_USDT"));
    EXPECT_DOUBLE_EQ(3000.0, pm.symbolNotional("ETH_USDT"));
    EXPECT_DOUBLE_EQ(0.0, pm.symbolNotional("SOL_USDT"));
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
                (void)pm.openPosition("BTC_USDT", Side::Buy, 0.001, 50000.0,
                    "thread_" + std::to_string(t));
            }
        });
    }

    threads.clear(); // Join all threads.
    EXPECT_EQ(kThreadCount * kOpensPerThread, pm.openPositionCount());
}

TEST(PositionManager, ConcurrentReadsAndWritesAreSafe)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    // Pre-open some positions.
    for (int i = 0; i < 10; ++i)
    {
        (void)pm.openPosition("BTC_USDT", Side::Buy, 0.001, 50000.0, "s1");
    }

    std::atomic<bool> stop{ false };

    // Reader thread: continuously reads portfolio summary.
    std::jthread reader([&pm, &stop]()
    {
        while (!stop.load())
        {
            const auto summary = pm.portfolioSummary();
            (void)summary; // Just exercise the read path.
        }
    });

    // Writer thread: updates prices.
    std::jthread writer([&pm, &stop]()
    {
        double price = 50000.0;
        while (!stop.load())
        {
            const auto positions = pm.getAllPositions();
            for (const auto &pos : positions)
            {
                pm.updatePrice(pos.position_id, price);
            }
            price += 100.0;
        }
    });

    // Let them run for a brief period.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
}

// ---------------------------------------------------------------------------
// M11: Futures position tests
// ---------------------------------------------------------------------------

TEST(PositionManager, FuturesPositionDefaultFields)
{
    // Default Position should have spot-compatible defaults.
    Position pos;
    EXPECT_EQ(MarketType::Spot, pos.market_type);
    EXPECT_DOUBLE_EQ(1.0, pos.leverage);
    EXPECT_EQ(MarginMode::Cross, pos.margin_mode);
    EXPECT_DOUBLE_EQ(0.0, pos.margin_used);
    EXPECT_DOUBLE_EQ(0.0, pos.liquidation_price);
    EXPECT_DOUBLE_EQ(1.0, pos.quanto_multiplier);
}

TEST(PositionManager, OpenFuturesPosition_SetsFields)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    auto result = pm.openPosition(
        "BTC_USDT", Side::Buy, 10, 50000.0, "scalper",
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005);

    ASSERT_TRUE(pulse::ok(result));

    const auto pos = pm.getPosition(pulse::value(result));
    ASSERT_TRUE(pos.has_value());
    EXPECT_EQ(MarketType::Futures, pos->market_type);
    EXPECT_DOUBLE_EQ(10.0, pos->leverage);
    EXPECT_EQ(MarginMode::Cross, pos->margin_mode);
    EXPECT_DOUBLE_EQ(0.0001, pos->quanto_multiplier);
}

TEST(PositionManager, OpenFuturesPosition_MarginCalculation)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    // margin = qty * entry * quanto / leverage = 10 * 50000 * 0.0001 / 10 = 5.0
    auto result = pm.openPosition(
        "BTC_USDT", Side::Buy, 10, 50000.0, "scalper",
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005);

    ASSERT_TRUE(pulse::ok(result));

    const auto pos = pm.getPosition(pulse::value(result));
    ASSERT_TRUE(pos.has_value());
    EXPECT_NEAR(5.0, pos->margin_used, 1e-9);
}

TEST(PositionManager, OpenFuturesPosition_LiquidationPriceBuy)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    // liq = entry * (1 - 1/leverage + maintenance_rate)
    //     = 50000 * (1 - 1/10 + 0.005) = 50000 * 0.905 = 45250
    auto result = pm.openPosition(
        "BTC_USDT", Side::Buy, 10, 50000.0, "scalper",
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005);

    ASSERT_TRUE(pulse::ok(result));

    const auto pos = pm.getPosition(pulse::value(result));
    ASSERT_TRUE(pos.has_value());
    EXPECT_NEAR(45250.0, pos->liquidation_price, 1.0);
}

TEST(PositionManager, OpenFuturesPosition_LiquidationPriceSell)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    // liq = entry * (1 + 1/leverage - maintenance_rate)
    //     = 50000 * (1 + 1/10 - 0.005) = 50000 * 1.095 = 54750
    auto result = pm.openPosition(
        "BTC_USDT", Side::Sell, 10, 50000.0, "scalper",
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005);

    ASSERT_TRUE(pulse::ok(result));

    const auto pos = pm.getPosition(pulse::value(result));
    ASSERT_TRUE(pos.has_value());
    EXPECT_NEAR(54750.0, pos->liquidation_price, 1.0);
}

TEST(PositionManager, UpdatePrice_FuturesPnlWithLeverage)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    // Open 10x leveraged buy: 10 contracts @ 50000, quanto=0.0001
    auto result = pm.openPosition(
        "BTC_USDT", Side::Buy, 10, 50000.0, "scalper",
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005);

    ASSERT_TRUE(pulse::ok(result));
    const std::string pos_id = pulse::value(result);

    // Price moves to 51000 (+2%)
    // PnL = (51000 - 50000) * 10 * 0.0001 * 10 = 1000 * 10 * 0.0001 * 10 = 10.0
    pm.updatePrice(pos_id, 51000.0);

    const auto pos = pm.getPosition(pos_id);
    ASSERT_TRUE(pos.has_value());
    EXPECT_NEAR(10.0, pos->unrealized_pnl, 1e-6);
}

TEST(PositionManager, CalculatePnl_SpotEquivalent)
{
    // Verify spot PnL is unchanged when using default leverage=1.0, quanto=1.0.
    // Open spot buy: 0.001 BTC @ 50000, price moves to 51000.
    // Expected PnL = (51000 - 50000) * 0.001 = 1.0
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    auto result = pm.openPosition("BTC_USDT", Side::Buy, 0.001, 50000.0, "s1");
    ASSERT_TRUE(pulse::ok(result));

    pm.updatePrice(pulse::value(result), 51000.0);

    const auto pos = pm.getPosition(pulse::value(result));
    ASSERT_TRUE(pos.has_value());
    EXPECT_NEAR(1.0, pos->unrealized_pnl, 1e-9);
}

TEST(PositionManager, PortfolioSummary_FuturesFields)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    // Open 1 spot + 1 futures position.
    (void)pm.openPosition("ETH_USDT", Side::Buy, 1.0, 3000.0, "s1");
    (void)pm.openPosition(
        "BTC_USDT", Side::Buy, 10, 50000.0, "s2",
        MarketType::Futures, 5.0, MarginMode::Cross, 0.0001, 0.005);

    const auto summary = pm.portfolioSummary();
    EXPECT_EQ(2, summary.openPositionCount);
    EXPECT_EQ(1, summary.futures_position_count);
    EXPECT_GT(summary.total_margin_used, 0.0);
}
