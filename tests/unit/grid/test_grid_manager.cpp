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
        auto &buf = m_harness->feed.getKlineBuffer("ETH_USDT");
        for (int i = 0; i < n; ++i)
        {
            market::Kline k;
            k.open = price;
            k.high = price + 0.5;
            k.low = price - 0.5;
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
