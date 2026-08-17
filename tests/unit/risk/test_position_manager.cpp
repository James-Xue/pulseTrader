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

// ---------------------------------------------------------------------------
// syncPositionFromExchange — startup reconciliation
// ---------------------------------------------------------------------------

TEST(PositionManager, SyncImportsExchangePositionWithoutLimitChecks)
{
    // Tiny limits on purpose: a real exchange position must be imported
    // even when it exceeds the configured caps (hiding it would undercount
    // risk, not protect it).
    PositionManager pm(make_config(100.0, 2, 50.0));

    // 6 contracts of BTC @ 63072.3 → notional ≈ 37.8 (quanto 0.0001) — under
    // the caps; margin_used = 6 * 63072.3 * 0.0001 / 10 ≈ 3.78.
    pm.syncPositionFromExchange(
        "BTC_USDT", Side::Sell, 6.0, 63072.3, 63037.2,
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005, 69000.0);

    const auto all = pm.getAllPositions();
    ASSERT_EQ(1, all.size());
    const auto &pos = all[0];

    EXPECT_EQ("BTC_USDT_Sell_sync", pos.position_id);
    EXPECT_EQ(Side::Sell, pos.side);
    EXPECT_NEAR(6.0, pos.quantity, 1e-9);
    EXPECT_NEAR(63072.3, pos.entry_price, 1e-9);
    EXPECT_NEAR(63037.2, pos.current_price, 1e-9);
    EXPECT_EQ(MarketType::Futures, pos.market_type);
    EXPECT_NEAR(10.0, pos.leverage, 1e-9);
    EXPECT_EQ(MarginMode::Cross, pos.margin_mode);
    EXPECT_NEAR(0.0001, pos.quanto_multiplier, 1e-12);
    EXPECT_NEAR(69000.0, pos.liquidation_price, 1e-9);
    // margin = qty * entry * quanto / leverage.
    EXPECT_NEAR(6.0 * 63072.3 * 0.0001 / 10.0, pos.margin_used, 1e-6);
    // Short at a mark below entry → positive unrealized PnL.
    EXPECT_NEAR((63072.3 - 63037.2) * 6.0 * 0.0001 * 10.0, pos.unrealized_pnl, 1e-6);
    // No owning strategy.
    EXPECT_TRUE(pos.strategy_id.empty());
}

TEST(PositionManager, SyncImportAboveLimitsStillVisible)
{
    PositionManager pm(make_config(50.0, 2, 20.0)); // tight caps

    // A real exchange position that exceeds every configured limit must
    // still be imported — the caps apply to NEW orders, not to reality.
    pm.syncPositionFromExchange(
        "ETH_USDT", Side::Buy, 100.0, 3000.0, 3010.0,
        MarketType::Futures, 5.0, MarginMode::Cross, 0.01, 0.005, 2500.0);

    const auto all = pm.getAllPositions();
    ASSERT_EQ(1, all.size());
    EXPECT_EQ("ETH_USDT_Buy_sync", all[0].position_id);
    EXPECT_NEAR(100.0, all[0].quantity, 1e-9);
}

TEST(PositionManager, SyncIsIdempotentPerSymbolSide)
{
    PositionManager pm(make_config());

    pm.syncPositionFromExchange(
        "BTC_USDT", Side::Sell, 6.0, 63072.3, 63037.2,
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005, 69000.0);

    // Re-sync with updated prices/qty (exchange merged position changed).
    pm.syncPositionFromExchange(
        "BTC_USDT", Side::Sell, 7.0, 63050.0, 63010.0,
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005, 69100.0);

    const auto all = pm.getAllPositions();
    ASSERT_EQ(1, all.size()); // updated, not duplicated
    EXPECT_EQ("BTC_USDT_Sell_sync", all[0].position_id);
    EXPECT_NEAR(7.0, all[0].quantity, 1e-9);
    EXPECT_NEAR(63050.0, all[0].entry_price, 1e-9);
    EXPECT_NEAR(69100.0, all[0].liquidation_price, 1e-9);
}

TEST(PositionManager, SyncDoesNotCollideWithEngineOpenedPositions)
{
    PositionManager pm(make_config(100000.0, 100, 100000.0));

    // Exchange-side synced short.
    pm.syncPositionFromExchange(
        "BTC_USDT", Side::Sell, 6.0, 63072.3, 63037.2,
        MarketType::Futures, 10.0, MarginMode::Cross, 0.0001, 0.005, 69000.0);

    // Engine opens a new position in the same symbol — separate id.
    const auto opened = pm.openPosition(
        "BTC_USDT", Side::Sell, 1.0, 63000.0, "momentum");
    ASSERT_TRUE(ok(opened));

    const auto all = pm.getAllPositions();
    ASSERT_EQ(2, all.size());
    EXPECT_NE("BTC_USDT_Sell_sync", pulse::value(opened));
    EXPECT_TRUE(pm.getPosition("BTC_USDT_Sell_sync").has_value());
    EXPECT_TRUE(pm.getPosition(pulse::value(opened)).has_value());
}

// ---------------------------------------------------------------------------
// Per-market notional budget (M17)
//
// The position-notional cap is enforced PER MARKET TYPE: optional
// maxPositionNotional{Futures,Cfd,Spot} overrides fall back to
// maxPositionNotional. A futures position must not consume the CFD budget —
// the 2026-08-17 incident: SKHY futures (~5099 USDT notional) clamped a
// 0.01-lot XAUUSD CFD order to 0.003 lots (below the 0.01 minimum).
// ---------------------------------------------------------------------------

namespace
{
// Config mirroring the live engine: fallback 1000, per-market 6000.
RiskConfig per_market_config()
{
    RiskConfig cfg = make_config(1000.0, 5, 6000.0);
    cfg.maxPositionNotionalFutures = 6000.0;
    cfg.maxPositionNotionalCfd = 6000.0;
    return cfg;
}

// Open a SKHY futures short of ~5099 USDT notional (3000 contracts,
// quanto 0.01), like the synced live position.
void open_skhy_futures(PositionManager &pm)
{
    const auto r = pm.openPosition(
        "SKHY_USDT", Side::Sell, 3000.0, 169.97, "manual",
        MarketType::Futures, 25.0, MarginMode::Cross, 0.01, 0.025);
    ASSERT_TRUE(ok(r));
    // Sanity: notional = 3000 * 169.97 * 0.01 = 5099.1
    ASSERT_NEAR(5099.1, pm.getPosition(value(r))->notional_value, 1e-6);
}
} // namespace

TEST(PositionManager, PerMarketNotional_FuturesDoesNotConsumeCfdBudget)
{
    PositionManager pm(per_market_config());
    open_skhy_futures(pm);

    // The 0.01-lot XAUUSD CFD order (4392 * 100 * 0.01 = 4392 USDT notional)
    // fits the CFD cap 6000 even though the futures cap is 5099/6000 used.
    const auto res = pm.reserveNotional("XAUUSD", 0.01, 4392.0, 100.0,
                                        MarketType::Cfd);
    ASSERT_TRUE(res.approved);
    EXPECT_EQ(RiskDecision::Approved, res.decision);
    EXPECT_DOUBLE_EQ(0.01, res.approved_qty);
}

TEST(PositionManager, PerMarketNotional_FuturesBudgetStillClampsFutures)
{
    PositionManager pm(per_market_config());
    open_skhy_futures(pm);

    // Same-market: 5099.1 used of 6000 → ~900.9 remains for futures.
    const auto res = pm.reserveNotional("SKHY_USDT", 1.0, 5099.0, 1.0,
                                        MarketType::Futures);
    ASSERT_TRUE(res.approved);
    EXPECT_EQ(RiskDecision::Modified, res.decision);
    EXPECT_NEAR(900.9 / 5099.0, res.approved_qty, 1e-6);
}

TEST(PositionManager, PerMarketNotional_OverridesHonoredPerMarket)
{
    RiskConfig cfg = make_config(1000.0, 5, 6000.0);
    cfg.maxPositionNotionalFutures = 6000.0;
    cfg.maxPositionNotionalCfd = 3000.0; // tighter CFD cap
    PositionManager pm(cfg);

    // 0.004 lots (1756.8 USDT) fits the CFD cap → Approved untouched.
    // (Checked first: a Modified reservation would consume the CFD budget.)
    const auto fits = pm.reserveNotional("XAUUSD", 0.004, 4392.0, 100.0,
                                         MarketType::Cfd);
    ASSERT_TRUE(fits.approved);
    EXPECT_EQ(RiskDecision::Approved, fits.decision);
    EXPECT_DOUBLE_EQ(0.004, fits.approved_qty);

    // CFD order 4392 > CFD cap 3000 → Modified to (3000-1756.8) / (4392*100),
    // i.e. the budget left after the pending reservation.
    const auto clamped = pm.reserveNotional("XAUUSD", 0.01, 4392.0, 100.0,
                                            MarketType::Cfd);
    ASSERT_TRUE(clamped.approved);
    EXPECT_EQ(RiskDecision::Modified, clamped.decision);
    EXPECT_NEAR((3000.0 - 1756.8) / (4392.0 * 100.0), clamped.approved_qty,
                1e-9);
}

TEST(PositionManager, PerMarketNotional_FallbackWhenUnset)
{
    // Only the global fallback set — the cap applies per market type with
    // maxPositionNotional as the value, so each bucket clamps identically.
    PositionManager pm(make_config(1000.0, 5, 1000.0));

    // Futures bucket: 900 used → 100 remains.
    const auto r = pm.openPosition(
        "BTC_USDT", Side::Buy, 1.0, 90000.0, "s1",
        MarketType::Futures, 10.0, MarginMode::Cross, 0.01, 0.005);
    ASSERT_TRUE(ok(r)); // 900 USDT notional.

    // 100 USDT remains; a 1000-USDT futures proposal → Modified to 0.1.
    const auto futures = pm.reserveNotional("BTC_USDT", 1.0, 100000.0, 0.01,
                                            MarketType::Futures);
    ASSERT_TRUE(futures.approved);
    EXPECT_EQ(RiskDecision::Modified, futures.decision);
    EXPECT_NEAR(100.0 / 1000.0, futures.approved_qty, 1e-9);

    // CFD bucket is empty → the same 1000-USDT proposal fits the fallback
    // cap untouched (the futures position does not consume it).
    const auto cfd_full = pm.reserveNotional("XAUUSD", 1.0, 100000.0, 0.01,
                                             MarketType::Cfd);
    ASSERT_TRUE(cfd_full.approved);
    EXPECT_EQ(RiskDecision::Approved, cfd_full.decision);
    EXPECT_DOUBLE_EQ(1.0, cfd_full.approved_qty);

    // Order not placed — release the reservation (realistic cancel path).
    pm.cancelReservation(cfd_full.reservation_id);

    // Fill the CFD bucket to 900 → the same 0.1 clamp applies there too.
    const auto cfd_pos = pm.openPosition(
        "XAUUSD", Side::Buy, 0.9, 1000.0, "s1",
        MarketType::Cfd, 500.0, MarginMode::Cross, 1.0, 0.005);
    ASSERT_TRUE(ok(cfd_pos)); // 900 USDT notional.

    const auto cfd = pm.reserveNotional("XAUUSD", 1.0, 100000.0, 0.01,
                                        MarketType::Cfd);
    ASSERT_TRUE(cfd.approved);
    EXPECT_EQ(RiskDecision::Modified, cfd.decision);
    EXPECT_NEAR(100.0 / 1000.0, cfd.approved_qty, 1e-9);
}

TEST(PositionManager, PerMarketNotional_OpenPositionValidatesPerMarket)
{
    PositionManager pm(per_market_config());
    open_skhy_futures(pm);

    // Fill-path re-validation must use the CFD cap, not the futures-occupied
    // total — otherwise the fill would be rejected with PositionLimitHit.
    const auto r = pm.openPosition(
        "XAUUSD", Side::Buy, 0.01, 4392.0, "cfd1",
        MarketType::Cfd, 500.0, MarginMode::Cross, 100.0, 0.005);
    ASSERT_TRUE(ok(r));
}

TEST(PositionManager, CanOpenPosition_PerMarketAware)
{
    PositionManager pm(per_market_config());
    open_skhy_futures(pm);

    // CFD pre-check passes (own budget), futures pre-check fails (cap used).
    EXPECT_TRUE(pm.canOpenPosition("XAUUSD", 0.01, 4392.0, 100.0,
                                   MarketType::Cfd));
    EXPECT_FALSE(pm.canOpenPosition("SKHY_USDT", 1.0, 5099.0, 1.0,
                                    MarketType::Futures));
}
