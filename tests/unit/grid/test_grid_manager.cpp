// test_grid_manager.cpp — M27 GridManager unit tests
//
// FakeGateway replaces the exchange (orders/positions); SignalBoard carries
// the eth_scalper trend gate; the futures feed is the FeedHarness pattern
// (no network I/O). tick(now_ms) is called with controlled timestamps so the
// daily boundary, spike freeze and re-anchor cooldown are all deterministic.

#include "grid/GridManager.hpp"

#include "exchange/GateRestClient.hpp"
#include "market/MarketFeed.hpp"
#include "strategy/signal/SignalBoard.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::grid;
using namespace pulse::strategy;

namespace
{

// ---------------------------------------------------------------------------
// FakeGateway — scripted exchange
// ---------------------------------------------------------------------------
class FakeGateway : public IGridGateway
{
  public:
    std::vector<ExchangeOrderView> live; ///< The exchange's open-order view.
    std::vector<PositionView> positions;
    std::vector<execution::OrderRequest> placed; ///< Every place() call.
    int cancel_count{ 0 };

    Result<execution::OrderResponse> place(const execution::OrderRequest &req) override
    {
        ExchangeOrderView v;
        v.order_id = "ord-" + std::to_string(placed.size() + 1);
        v.client_order_id = req.client_order_id;
        v.price = req.price;
        v.quantity = req.reduce_only ? req.quantity : -req.quantity;
        v.reduce_only = req.reduce_only;
        v.status = "open";
        live.push_back(v);

        execution::OrderResponse resp;
        resp.order_id = v.order_id;
        resp.status = OrderStatus::Open;
        placed.push_back(req);
        return resp;
    }

    bool cancel(const std::string &order_id) override
    {
        ++cancel_count;
        live.erase(std::remove_if(live.begin(), live.end(),
                      [&](const ExchangeOrderView &o)
                      { return o.order_id == order_id; }),
                   live.end());
        return true;
    }

    std::vector<ExchangeOrderView> openFuturesOrders(const std::string &) override
    {
        return live;
    }

    std::vector<PositionView> positionsBySymbol(const std::string &) override
    {
        return positions;
    }

    /// Simulate a fill: the order vanishes from the exchange view.
    void simulateFill(const std::string &order_id)
    {
        live.erase(std::remove_if(live.begin(), live.end(),
                      [&](const ExchangeOrderView &o)
                      { return o.order_id == order_id; }),
                   live.end());
    }
};

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
class GridManagerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_cfg.symbol = "ETH_USDT";
        m_cfg.levels = 4; // small grid for test speed
        m_cfg.qty_per_level = 2.0;
        m_cfg.step_mode = "fixed";
        m_cfg.step_fixed = 5.0;
        m_cfg.state_file = "grid_test_state.json";
        m_cfg.force = true;

        m_harness = std::make_unique<FeedHarness>(MarketType::Futures);
        m_board = std::make_unique<SignalBoard>(0.7);
        m_manager = std::make_unique<GridManager>(
            m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    }

    void TearDown() override
    {
        std::filesystem::remove_all(m_stateDir);
    }

    /// Push a flat 1m candle series (price p, range ±0.5).
    void pushCandles(double price, int n = 50)
    {
        pushCandlesRange(price, 0.5, n);
    }

    /// Flat candles with a custom range (±range; TR = 2×range).
    void pushCandlesRange(double price, double range, int n = 50)
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        for (int i = 0; i < n; ++i)
        {
            market::Kline k;
            k.open = price;
            k.high = price + range;
            k.low = price - range;
            k.close = price;
            k.closed = true;
            buf.push(k);
        }
    }

    /// Set the current mid via the ticker cache.
    void setMid(double price)
    {
        market::Ticker t;
        t.symbol = "ETH_USDT";
        t.bid = price - 0.01;
        t.ask = price + 0.01;
        t.last = price;
        m_harness->feed.tickerCache().update("ETH_USDT", t);
    }

    /// Publish the eth_scalper trend gate to the SignalBoard.
    void setTrendGate(const std::string &state)
    {
        TradingSignal sig;
        sig.strategy_id = "eth_scalper_ETH_USDT";
        sig.symbol = "ETH_USDT";
        sig.type = SignalType::Flat;
        sig.confidence = 0.0;
        sig.indicators = { { "trend_state", state }, { "spike", 0 } };
        m_board->publish(sig);
    }

    /// Drive `n` ticks; the slow pass runs every 250 ticks.
    void driveTicks(int n, std::int64_t now_ms)
    {
        for (int i = 0; i < n; ++i)
        {
            m_manager->tick(now_ms + i * 200);
        }
    }

    // Need a harness with a real (no-I/O) feed — same pattern as the
    // EthScalper tests.
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

    GridConfig m_cfg;
    FakeGateway m_gateway;
    std::unique_ptr<FeedHarness> m_harness;
    std::unique_ptr<SignalBoard> m_board;
    std::unique_ptr<GridManager> m_manager;
    std::mutex m_restMutex;
    std::filesystem::path m_stateDir{ "data_grid_test" };

    std::int64_t baseMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, StartRefusesWhenUserPositionExists)
{
    m_cfg.force = false;
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    m_gateway.positions.push_back(
        { "ETH_USDT", "", 10.0, 2000.0, 200.0, true }); // user's short

    const auto res = m_manager->start(nlohmann::json::object());
    ASSERT_FALSE(ok(res));
    EXPECT_NE(std::string::npos, error(res).message.find("refused"));
}

TEST_F(GridManagerTest, StartHangsAllLevelsOnSlowPass)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");

    const auto start = m_manager->start(nlohmann::json::object());
    ASSERT_TRUE(ok(start));
    EXPECT_EQ(GridPhase::Running, value(start).phase);

    driveTicks(250, baseMs()); // first slow pass

    // 4 levels hung above the mid: anchor = round(2001, 5) = 2000... wait:
    // anchor = round(mid + 1×step, step) = round(2005, 5) = 2005.
    ASSERT_EQ(4u, m_gateway.placed.size());
    for (std::size_t i = 0; i < m_gateway.placed.size(); ++i)
    {
        const auto &req = m_gateway.placed[i];
        EXPECT_EQ(Side::Sell, req.side);
        EXPECT_EQ(2.0, req.quantity);
        EXPECT_EQ(OrderType::Limit, req.type);
        EXPECT_EQ(MarketType::Futures, req.market_type);
        EXPECT_FALSE(req.reduce_only);
        EXPECT_EQ(2005.0 + static_cast<double>(i) * 5.0, req.price);
        EXPECT_EQ(0u, req.client_order_id.rfind("eth-grid-sell-", 0));
    }
}

// ---------------------------------------------------------------------------
// TP lifecycle
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, FillHangsReduceOnlyTp)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());
    const auto sells = m_gateway.placed.size();
    ASSERT_EQ(4u, sells);

    // Level 2005 fills (whole level = 2 contracts in one order).
    const auto &level_order = m_gateway.live[0];
    m_gateway.simulateFill(level_order.order_id);
    driveTicks(250, baseMs() + 60000); // next slow pass

    ASSERT_EQ(5u, m_gateway.placed.size()); // 4 sells + 1 TP
    const auto &tp = m_gateway.placed.back();
    EXPECT_EQ(Side::Buy, tp.side);
    EXPECT_EQ(2.0, tp.quantity);
    EXPECT_TRUE(tp.reduce_only); // M27: reduce-only, never a trigger order
    EXPECT_EQ(1995.0, tp.price); // fill 2005 - 2×step
    EXPECT_EQ(0u, tp.client_order_id.rfind("eth-grid-tp-", 0));
}

TEST_F(GridManagerTest, TpFillRecordsPnlAndRehangs)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());

    // Fill level 2005, then fill its TP.
    const auto &sell = m_gateway.live[0];
    m_gateway.simulateFill(sell.order_id);
    driveTicks(250, baseMs() + 60000);
    ASSERT_EQ(5u, m_gateway.placed.size()); // 4 sells + 1 TP
    const auto tp_it = std::find_if(m_gateway.live.begin(), m_gateway.live.end(),
        [](const ExchangeOrderView &o) { return o.reduce_only; });
    ASSERT_NE(m_gateway.live.end(), tp_it);
    EXPECT_TRUE(tp_it->reduce_only);
    m_gateway.simulateFill(tp_it->order_id);
    setTrendGate("bearish"); // keep the gate fresh across the 2-min jump
    driveTicks(250, baseMs() + 120000);

    // PnL: (2005 - 1995) × 2 contracts × 0.01 ETH = 0.20 USD.
    const auto snap = m_manager->status();
    EXPECT_NEAR(0.20, snap.realized_pnl_today, 1e-9);
    // The level was re-hung for the next cycle: 4 sells + 1 TP + 1 re-hung.
    EXPECT_EQ(0, snap.levels_filled);
    EXPECT_EQ(6u, m_gateway.placed.size());
}

// ---------------------------------------------------------------------------
// Trend gate
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, BullishTrendBlocksNewLevels)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bullish"); // uptrend — no new levels

    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());

    EXPECT_TRUE(m_gateway.placed.empty()); // nothing hung in an uptrend
}

// ---------------------------------------------------------------------------
// Spike freeze
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, SpikeFreezesNewLevelsThenExpires)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));

    // Spike: last 1m candle +3% (60 USD on 2000) — well past 1% and 3×ATR.
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        market::Kline k;
        k.open = 2000.0;
        k.high = 2065.0;
        k.low = 1999.5;
        k.close = 2060.0;
        k.closed = true;
        buf.push(k);
    }
    driveTicks(5, baseMs()); // fast/mid layer detects the spike

    const auto frozen = m_manager->status();
    EXPECT_TRUE(frozen.spike_frozen);

    // New levels stay frozen during the freeze window.
    driveTicks(250, baseMs() + 1000);
    EXPECT_TRUE(m_gateway.placed.empty());

    // After the freeze expires, levels hang again (bearish trend intact;
    // re-publish so the signal-board timestamp is fresh after 40 min, and
    // push a calm candle so the spike detector stops re-tripping).
    setTrendGate("bearish");
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        market::Kline k;
        k.open = 2000.0;
        k.high = 2000.5;
        k.low = 1999.5;
        k.close = 2000.0;
        k.closed = true;
        buf.push(k);
    }
    driveTicks(250, baseMs() + 40 * 60000);
    EXPECT_EQ(4u, m_gateway.placed.size());
}

// ---------------------------------------------------------------------------
// Protection line A
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, ProtectLineAFlattensGridShare)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());

    // Fill one level (2005) so there is a grid share to protect.
    const auto &sell = m_gateway.live[0];
    m_gateway.simulateFill(sell.order_id);
    driveTicks(250, baseMs() + 60000);
    ASSERT_EQ(5u, m_gateway.placed.size()); // 4 sells + 1 TP

    // 1m close blasts above top + 2×step (top = 2020, line = 2030).
    setMid(2036.0); // market also moved up with the candle
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        market::Kline k;
        k.open = 2020.0;
        k.high = 2041.0;
        k.low = 2019.0;
        k.close = 2036.0;
        k.closed = true;
        buf.push(k);
    }
    driveTicks(250, baseMs() + 120000);

    const auto flat_it = std::find_if(m_gateway.placed.begin(), m_gateway.placed.end(),
        [](const execution::OrderRequest &r)
        { return r.client_order_id.rfind("eth-grid-flatten-", 0) == 0; });
    ASSERT_NE(m_gateway.placed.end(), flat_it);
    EXPECT_EQ(Side::Buy, flat_it->side);
    EXPECT_TRUE(flat_it->reduce_only);
    EXPECT_EQ(2.0, flat_it->quantity); // exactly the grid share (1 level × 2)
}

// ---------------------------------------------------------------------------
// Daily loss stop + reset
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, DailyLossFreezesOnProtectFlattenAndResetsAtBoundary)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    m_cfg.daily_loss_limit_usd = -0.5; // tight for the test
    // m_cfg is a value copy inside the manager — rebuild with the tight limit.
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());

    // Fill one level so there is a share to lose on a protection flatten.
    const auto &sell = m_gateway.live[0];
    m_gateway.simulateFill(sell.order_id);
    driveTicks(250, baseMs() + 60000);

    // 1m close above top + 2×step → protection A flattens at a loss:
    // (2005 - 2036) × 2 × 0.01 = -0.62 ≤ -0.50 → daily freeze.
    setMid(2036.0);
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        market::Kline k;
        k.open = 2020.0;
        k.high = 2041.0;
        k.low = 2019.0;
        k.close = 2036.0;
        k.closed = true;
        buf.push(k);
    }
    driveTicks(250, baseMs() + 120000);

    const auto frozen = m_manager->status();
    EXPECT_TRUE(frozen.daily_loss_frozen);
    EXPECT_NEAR(-0.62, frozen.realized_pnl_today, 1e-9);

    // Jump past the next UTC midnight (Beijing 08:00 boundary): the window
    // rolls and the freeze lifts.
    const std::int64_t next_day = (baseMs() / 86400000 + 1) * 86400000 + 3600000;
    driveTicks(5, next_day);
    const auto snap = m_manager->status();
    EXPECT_DOUBLE_EQ(0.0, snap.realized_pnl_today); // rolled
    EXPECT_FALSE(snap.daily_loss_frozen);
}

// ---------------------------------------------------------------------------
// Re-anchor
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, ReanchorFollowsPriceDownWithCooldown)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());

    // Price falls far below the grid (anchor 2005, lower re-anchor line
    // 2005 - 3×5 = 1990): mid 1980 → follow down.
    setMid(1980.0);
    driveTicks(250, baseMs() + 30000);

    // Re-anchor: all orders cancelled, levels re-hung at the new anchor.
    EXPECT_GT(m_gateway.cancel_count, 0);
    const auto snap = m_manager->status();
    EXPECT_DOUBLE_EQ(1985.0, snap.anchor); // round(1981, 5)
}

// ---------------------------------------------------------------------------
// Direction switch
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, DirectionSwitchSweepIsNotAFill)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());

    // Engine direction switch sweeps ALL futures orders (external cancel).
    m_gateway.live.clear();
    m_manager->onDirectionSwitched();
    driveTicks(250, baseMs() + 60000);

    // The swept orders must NOT count as fills; levels re-hang instead.
    const auto snap = m_manager->status();
    EXPECT_EQ(0, snap.levels_filled);
    EXPECT_EQ(8u, m_gateway.placed.size()); // 4 original + 4 re-hung
}

// ---------------------------------------------------------------------------
// Start overrides (PR-6)
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, StartAppliesLevelsOverride)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");

    const auto start = m_manager->start(nlohmann::json{ { "levels", 6 } });
    ASSERT_TRUE(ok(start));
    const auto snap = value(start);
    EXPECT_EQ(6u, snap.levels.size());
    EXPECT_DOUBLE_EQ(2005.0, snap.anchor); // round(2000 + 1×5, 5)
    EXPECT_DOUBLE_EQ(2035.0, snap.top);    // 2005 + 6×5

    driveTicks(250, baseMs()); // first slow pass hangs all 6
    ASSERT_EQ(6u, m_gateway.placed.size());
}

TEST_F(GridManagerTest, StartAppliesQtyOverride)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json{ { "qty_per_level", 4.0 } })));
    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());
    for (const auto &req : m_gateway.placed)
    {
        EXPECT_EQ(4.0, req.quantity);
    }
}

TEST_F(GridManagerTest, StartAppliesStepOverride)
{
    setMid(2000.0);
    pushCandles(2000.0); // flat candles → ATR would give step 3, override wins
    setTrendGate("bearish");

    const auto start = m_manager->start(nlohmann::json{ { "step", 10.0 } });
    ASSERT_TRUE(ok(start));
    const auto snap = value(start);
    EXPECT_DOUBLE_EQ(10.0, snap.step);
    EXPECT_DOUBLE_EQ(2010.0, snap.anchor); // round(2000 + 1×10, 10)

    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());
    EXPECT_DOUBLE_EQ(2010.0, m_gateway.placed[0].price);
    EXPECT_DOUBLE_EQ(2020.0, m_gateway.placed[1].price);
}

TEST_F(GridManagerTest, StartAppliesAnchorOverride)
{
    // Mid sits near the override so the grid does not instantly trigger the
    // follow-down re-anchor (mid < anchor - 3×step would move it away).
    setMid(2095.0);
    pushCandles(2095.0);
    setTrendGate("bearish");

    const auto start = m_manager->start(nlohmann::json{ { "anchor", 2100.0 } });
    ASSERT_TRUE(ok(start));
    const auto snap = value(start);
    EXPECT_DOUBLE_EQ(2100.0, snap.anchor); // round(2100, 5)

    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());
    EXPECT_DOUBLE_EQ(2100.0, m_gateway.placed[0].price); // 2100 + 5k
}

TEST_F(GridManagerTest, StartRejectsInvalidOverrides)
{
    setMid(2000.0);
    pushCandles(2000.0);

    const auto bad_levels = m_manager->start(nlohmann::json{ { "levels", 0 } });
    ASSERT_FALSE(ok(bad_levels));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(bad_levels).code);

    const auto bad_qty = m_manager->start(nlohmann::json{ { "qty_per_level", 0.0 } });
    ASSERT_FALSE(ok(bad_qty));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(bad_qty).code);

    const auto bad_step = m_manager->start(nlohmann::json{ { "step", -1.0 } });
    ASSERT_FALSE(ok(bad_step));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(bad_step).code);

    const auto bad_anchor = m_manager->start(nlohmann::json{ { "anchor", 0.0 } });
    ASSERT_FALSE(ok(bad_anchor));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(bad_anchor).code);

    const auto bad_type = m_manager->start(nlohmann::json{ { "levels", "x" } });
    ASSERT_FALSE(ok(bad_type));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(bad_type).code);

    // Still Disabled after every rejection — no partial geometry applied.
    EXPECT_EQ(GridPhase::Disabled, m_manager->status().phase);
}

TEST_F(GridManagerTest, ReanchorPreservesOverriddenGeometry)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(
        nlohmann::json{ { "levels", 6 }, { "qty_per_level", 4.0 } })));
    driveTicks(250, baseMs());
    ASSERT_EQ(6u, m_gateway.placed.size());

    // Price falls far below the grid (anchor 2005, lower line 1990) → follow
    // down — but the overridden geometry (6 levels × qty 4) must survive.
    setMid(1980.0);
    driveTicks(250, baseMs() + 30000);

    EXPECT_GT(m_gateway.cancel_count, 0);
    const auto snap = m_manager->status();
    EXPECT_DOUBLE_EQ(1985.0, snap.anchor); // round(1981, 5)
    EXPECT_EQ(6u, snap.levels.size());
    ASSERT_EQ(12u, m_gateway.placed.size()); // 6 + 6 re-hung
    for (std::size_t i = 6; i < m_gateway.placed.size(); ++i)
    {
        EXPECT_EQ(4.0, m_gateway.placed[i].quantity);
    }
}

// ---------------------------------------------------------------------------
// Re-anchor gates (v2 §3.1d / §4)
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, ReanchorRequiresBearishGate)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bullish"); // uptrend — nothing hung, nothing re-anchors
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());
    ASSERT_TRUE(m_gateway.placed.empty());

    // Price falls below the re-anchor line, but the bullish gate holds.
    setMid(1980.0);
    driveTicks(250, baseMs() + 30000);

    EXPECT_EQ(0, m_gateway.cancel_count);
    EXPECT_TRUE(m_gateway.placed.empty());
    EXPECT_STREQ("FreezeNew", m_manager->status().last_action.c_str());
}

TEST_F(GridManagerTest, SpikeFreezeBlocksReanchor)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());

    // Spike: 1m candle +3% (60 USD on 2000) → new levels frozen 30 min.
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        market::Kline k;
        k.open = 2000.0;
        k.high = 2065.0;
        k.low = 1999.5;
        k.close = 2060.0;
        k.closed = true;
        buf.push(k);
    }
    driveTicks(5, baseMs() + 60000);
    EXPECT_TRUE(m_manager->status().spike_frozen);

    // Price falls below the re-anchor line — the spike freeze blocks the
    // follow-down re-anchor too (v2 §3.1d covers 重锚).
    setMid(1980.0);
    driveTicks(250, baseMs() + 120000);
    EXPECT_EQ(0, m_gateway.cancel_count);
    EXPECT_EQ(4u, m_gateway.placed.size());

    // Freeze expires; price still below the line → re-anchor now happens.
    setTrendGate("bearish"); // keep the board fresh
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        market::Kline k;
        k.open = 1980.0;
        k.high = 1980.5;
        k.low = 1979.5;
        k.close = 1980.0;
        k.closed = true;
        buf.push(k); // calm candle so the spike detector stops re-tripping
    }
    driveTicks(250, baseMs() + 31 * 60000);

    EXPECT_GT(m_gateway.cancel_count, 0);
    EXPECT_EQ(8u, m_gateway.placed.size()); // 4 original + 4 re-hung
}

TEST_F(GridManagerTest, DailyStopFreezesAndBlocksReanchor)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    m_cfg.daily_loss_limit_usd = -0.5; // tight for the test
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());

    // Fill one level so a protection flatten realizes a loss.
    const auto &sell = m_gateway.live[0];
    m_gateway.simulateFill(sell.order_id);
    driveTicks(250, baseMs() + 60000);
    ASSERT_EQ(5u, m_gateway.placed.size()); // 4 sells + 1 TP

    // Protection A flattens at a loss: (2005 - 2036) × 2 × 0.01 = -0.62 ≤ -0.5.
    setMid(2036.0);
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        market::Kline k;
        k.open = 2020.0;
        k.high = 2041.0;
        k.low = 2019.0;
        k.close = 2036.0;
        k.closed = true;
        buf.push(k);
    }
    driveTicks(250, baseMs() + 120000);

    const auto frozen = m_manager->status();
    EXPECT_TRUE(frozen.daily_loss_frozen);
    EXPECT_NEAR(-0.62, frozen.realized_pnl_today, 1e-9);
    // The post-protection re-hang is held by the daily stop (v2 §4): the
    // flatten order is the only new order.
    EXPECT_EQ(6u, m_gateway.placed.size()); // 4 sells + 1 TP + 1 flatten

    // Price falls below the re-anchor line — the daily stop blocks the
    // follow-down re-anchor (no new positions/重挂 while frozen).
    setMid(1980.0);
    {
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        market::Kline k;
        k.open = 1980.0;
        k.high = 1980.5;
        k.low = 1979.5;
        k.close = 1980.0;
        k.closed = true;
        buf.push(k); // calm close so protection line A stops tripping
    }
    driveTicks(250, baseMs() + 180000);

    EXPECT_EQ(6u, m_gateway.placed.size());
    EXPECT_STREQ("DailyStop", m_manager->status().last_action.c_str());
}

// ---------------------------------------------------------------------------
// Protection line B
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, ProtectLineBFlattensGridShare)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());

    // Fill one level so there is a grid share whose floating loss can breach B.
    const auto &sell = m_gateway.live[0];
    m_gateway.simulateFill(sell.order_id);
    driveTicks(250, baseMs() + 60000);
    ASSERT_EQ(5u, m_gateway.placed.size()); // 4 sells + 1 TP

    // Mid blasts far above the fills: (2005 - 4000) × 2 × 0.01 = -39.9 ≤ -30.
    // The 1m close stays calm, so protection line A does NOT trip first.
    setMid(4000.0);
    driveTicks(250, baseMs() + 120000);

    const auto flat_it = std::find_if(m_gateway.placed.begin(),
        m_gateway.placed.end(),
        [](const execution::OrderRequest &r)
        { return r.client_order_id.rfind("eth-grid-flatten-", 0) == 0; });
    ASSERT_NE(m_gateway.placed.end(), flat_it);
    EXPECT_EQ(Side::Buy, flat_it->side);
    EXPECT_TRUE(flat_it->reduce_only);
    EXPECT_EQ(2.0, flat_it->quantity); // exactly the grid share (1 level × 2)
}

// ---------------------------------------------------------------------------
// Pause / resume
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, PauseKeepsOrdersAndBlocksActions)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());
    const int live_before = static_cast<int>(m_gateway.live.size());

    const auto pause = m_manager->pause();
    ASSERT_TRUE(ok(pause));
    EXPECT_EQ(GridPhase::Paused, value(pause).phase);

    // Orders stay on the exchange; no new actions while paused.
    EXPECT_EQ(live_before, static_cast<int>(m_gateway.live.size()));
    driveTicks(250, baseMs() + 60000);
    EXPECT_EQ(4u, m_gateway.placed.size());
    EXPECT_EQ(live_before, static_cast<int>(m_gateway.live.size()));
}

TEST_F(GridManagerTest, ResumeRestoresRunning)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());

    ASSERT_TRUE(ok(m_manager->pause()));
    driveTicks(250, baseMs() + 60000); // paused: nothing happens

    // A level fills while paused — the exchange reports it, the pause holds.
    m_gateway.simulateFill(m_gateway.live[0].order_id);

    const auto resume = m_manager->resume();
    ASSERT_TRUE(ok(resume));
    EXPECT_EQ(GridPhase::Running, value(resume).phase);

    // The next slow pass reconciles the fill and hangs the TP.
    driveTicks(250, baseMs() + 120000);
    EXPECT_EQ(5u, m_gateway.placed.size()); // 4 sells + 1 TP
}

TEST_F(GridManagerTest, ResumeRejectedWhenNotPaused)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");

    const auto disabled = m_manager->resume(); // Disabled
    ASSERT_FALSE(ok(disabled));
    EXPECT_EQ(ErrorCode::GridNotPaused, error(disabled).code);

    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));

    const auto running = m_manager->resume(); // Running
    ASSERT_FALSE(ok(running));
    EXPECT_EQ(ErrorCode::GridNotPaused, error(running).code);
}

// ---------------------------------------------------------------------------
// ATR adaptive step
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, AtrStepModeClamps)
{
    m_cfg.step_mode = "atr";
    m_cfg.step_min = 3.0;
    m_cfg.step_max = 8.0;

    // TR 10 (range 5) → raw 5 → step 5 (mid-range, no clamp).
    setMid(2000.0);
    pushCandlesRange(2000.0, 5.0);
    setTrendGate("bearish");
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    EXPECT_DOUBLE_EQ(5.0, m_manager->status().step);

    // TR 4 (range 2) → raw 2 → clamped up to step 3.
    setMid(2000.0);
    pushCandlesRange(2000.0, 2.0);
    setTrendGate("bearish");
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    EXPECT_DOUBLE_EQ(3.0, m_manager->status().step);

    // TR 30 (range 15) → raw 15 → clamped down to step 8.
    setMid(2000.0);
    pushCandlesRange(2000.0, 15.0);
    setTrendGate("bearish");
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    EXPECT_DOUBLE_EQ(8.0, m_manager->status().step);
}

// ---------------------------------------------------------------------------
// Persistence (schema 2 — PR-7)
// ---------------------------------------------------------------------------

TEST_F(GridManagerTest, SaveLoadRoundTripRestoresPausedState)
{
    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    ASSERT_TRUE(ok(m_manager->start(nlohmann::json::object())));
    driveTicks(250, baseMs());
    ASSERT_EQ(4u, m_gateway.placed.size());

    // Fill one level, hang its TP, then pause — the pause must persist so a
    // restart does NOT auto-resume a grid the operator deliberately held.
    const auto &sell = m_gateway.live[0];
    m_gateway.simulateFill(sell.order_id);
    driveTicks(250, baseMs() + 60000);
    ASSERT_TRUE(ok(m_manager->pause()));
    const auto before = m_manager->status();

    // New manager on the same state dir — as after an engine restart.
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    ASSERT_TRUE(m_manager->loadState());

    const auto after = m_manager->status();
    EXPECT_EQ(GridPhase::Paused, after.phase);
    EXPECT_DOUBLE_EQ(before.anchor, after.anchor);
    EXPECT_DOUBLE_EQ(before.step, after.step);
    EXPECT_DOUBLE_EQ(before.realized_pnl_today, after.realized_pnl_today);
    ASSERT_EQ(before.levels.size(), after.levels.size());
    for (std::size_t i = 0; i < before.levels.size(); ++i)
    {
        EXPECT_DOUBLE_EQ(before.levels[i].price, after.levels[i].price);
        EXPECT_EQ(before.levels[i].filled, after.levels[i].filled);
        EXPECT_DOUBLE_EQ(before.levels[i].fill_price,
                         after.levels[i].fill_price);
        EXPECT_EQ(before.levels[i].tp_resting, after.levels[i].tp_resting);
    }
}

TEST_F(GridManagerTest, RestoredRunningResumesAndAdopts)
{
    // Hand-write a schema-2 state: Running, level 0 filled with its TP still
    // resting on the exchange (crash survivor), level 1 filled with NO TP
    // order (the crash hit between fill and TP placement).
    std::filesystem::create_directories(m_stateDir);
    {
        std::ofstream out{ m_stateDir / "grid_test_state.json" };
        out << nlohmann::json{
            { "schema", 2 },
            { "phase", 2 }, // Running
            { "anchor", 2005.0 },
            { "step", 5.0 },
            { "realized_pnl_today", 0.5 },
            { "day_start_sec", (baseMs() / 1000 / 86400) * 86400 },
            { "spike_until_ms", 0 },
            { "reanchor_until_ms", 0 },
            { "daily_frozen", false },
            { "levels", nlohmann::json::array({
                  { { "price", 2005.0 }, { "filled", 2 },
                    { "fill_price", 2005.0 }, { "tp_filled", false },
                    { "tp_resting", true } },
                  { { "price", 2010.0 }, { "filled", 2 },
                    { "fill_price", 2010.0 }, { "tp_filled", false },
                    { "tp_resting", false } },
                  { { "price", 2015.0 }, { "filled", 0 },
                    { "fill_price", 0.0 }, { "tp_filled", false },
                    { "tp_resting", false } },
                  { { "price", 2020.0 }, { "filled", 0 },
                    { "fill_price", 0.0 }, { "tp_filled", false },
                    { "tp_resting", false } },
              }) },
        }.dump(2);
    }
    // The exchange still holds the level-0 TP (eth-grid-tp-1995 = 2005-2×5).
    m_gateway.live.push_back(
        { "ord-9", "eth-grid-tp-1995", 1995.0, 2.0, true, "open" });

    setMid(2000.0);
    pushCandles(2000.0);
    setTrendGate("bearish");
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    ASSERT_TRUE(m_manager->loadState());
    EXPECT_EQ(GridPhase::Running, m_manager->status().phase);
    EXPECT_NEAR(0.5, m_manager->status().realized_pnl_today, 1e-9);

    driveTicks(250, baseMs());

    // The adopted TP counts as resting (no duplicate), the missing TP for
    // the level-1 fill is re-hung, and the two free levels re-hang.
    const auto snap = m_manager->status();
    ASSERT_EQ(4u, snap.levels.size());
    EXPECT_TRUE(snap.levels[0].tp_resting);
    EXPECT_TRUE(snap.levels[1].tp_resting); // re-hung this pass
    const auto tp_it = std::find_if(m_gateway.placed.begin(),
        m_gateway.placed.end(),
        [](const execution::OrderRequest &r)
        { return r.client_order_id.rfind("eth-grid-tp-", 0) == 0; });
    ASSERT_NE(m_gateway.placed.end(), tp_it);
    EXPECT_DOUBLE_EQ(2000.0, tp_it->price); // 2010 - 2×5
    EXPECT_EQ(3u, m_gateway.placed.size()); // TP + 2 re-hung sells (2015/2020)
}

TEST_F(GridManagerTest, LegacyStateFileRestoresPaused)
{
    // schema=1 (pre-PR-7): the phase is never auto-activated — an operator
    // starts the grid explicitly after an upgrade.
    std::filesystem::create_directories(m_stateDir);
    {
        std::ofstream out{ m_stateDir / "grid_test_state.json" };
        out << nlohmann::json{
            { "schema", 1 },
            { "anchor", 2005.0 },
            { "step", 5.0 },
            { "realized_pnl_today", 0.0 },
            { "day_start_sec", (baseMs() / 1000 / 86400) * 86400 },
            { "spike_until_ms", 0 },
            { "reanchor_until_ms", 0 },
            { "daily_frozen", false },
            { "levels", nlohmann::json::array({
                  { { "price", 2005.0 }, { "filled", 2 },
                    { "fill_price", 2005.0 }, { "tp_filled", false } },
                  { { "price", 2010.0 }, { "filled", 0 },
                    { "fill_price", 0.0 }, { "tp_filled", false } },
              }) },
        }.dump(2);
    }
    setMid(2000.0);
    pushCandles(2000.0);
    m_manager = std::make_unique<GridManager>(
        m_cfg, m_gateway, *m_board, &m_harness->feed, m_restMutex, m_stateDir);
    ASSERT_TRUE(m_manager->loadState());

    // Restored as Paused — the state is kept, nothing trades until the
    // operator resumes/starts.
    EXPECT_EQ(GridPhase::Paused, m_manager->status().phase);
    driveTicks(250, baseMs());
    EXPECT_TRUE(m_gateway.placed.empty());
    EXPECT_EQ(2, m_manager->status().levels_filled);
}
