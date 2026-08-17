// test_order_flow_executor.cpp — Unit tests for OrderFlowExecutor
//
// Uses a FakeOrderPlacer so no network is involved; OrderTracker is
// driven via its testSimulateWsUpdate() interface.

#include "control/OrderFlowExecutor.hpp"

#include "execution/OrderTracker.hpp"
#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"
#include "market/OrderBookManager.hpp"
#include "market/SymbolRegistry.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/OrderRateLimiter.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace pulse;
using namespace pulse::control;
using namespace pulse::execution;
using namespace pulse::risk;
using namespace pulse::strategy;
using namespace pulse::exchange;

namespace
{

// ---------------------------------------------------------------------------
// FakeOrderPlacer — records place/cancel calls, returns scripted results
// ---------------------------------------------------------------------------
class FakeOrderPlacer : public IOrderPlacer
{
  public:
    int place_count{ 0 };
    int cancel_count{ 0 };
    int leverage_count{ 0 };
    std::string last_leverage_contract;
    double last_leverage_value{ 0.0 };
    bool fail_leverage{ false };
    bool fail_cancel{ false };   // Simulates an exchange that refuses the cancel
                                 // (order already filled / rejected).
    std::vector<OrderRequest> placed;
    std::optional<Result<OrderResponse>> scripted_result;

    Result<OrderResponse> place(const OrderRequest &req) override
    {
        ++place_count;
        placed.push_back(req);
        if (scripted_result.has_value())
        {
            // Scripted results are one-shot (like a real exchange response).
            auto result = *scripted_result;
            scripted_result.reset();
            return result;
        }
        OrderResponse resp;
        resp.order_id = "fake_" + std::to_string(place_count);
        resp.status = OrderStatus::Open;
        return resp;
    }

    bool cancel(const std::string &) override
    {
        ++cancel_count;
        return !fail_cancel;
    }

    Result<nlohmann::json> setLeverage(const std::string &contract,
                                       double leverage) override
    {
        ++leverage_count;
        last_leverage_contract = contract;
        last_leverage_value = leverage;
        if (fail_leverage)
        {
            return PulseError{ ErrorCode::HttpError, "leverage api down" };
        }
        return nlohmann::json{ { "ok", true } };
    }
};

// ---------------------------------------------------------------------------
// Fixture — realistic risk stack with dummy exchange clients
// ---------------------------------------------------------------------------
class OrderFlowTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_wsClient = std::make_unique<GateWsClient>(m_config);
        m_restClient = std::make_unique<GateRestClient>(m_config);
        m_tracker = std::make_unique<OrderTracker>(m_wsClient.get(), *m_restClient,
                                                   MarketType::Futures);
        m_cfdTracker = std::make_unique<OrderTracker>(nullptr, *m_restClient,
                                                      MarketType::Cfd, /*enable_ws=*/false);
        m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
        m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
        m_rateLimiter = std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
        m_riskMgr = std::make_unique<RiskManager>(m_riskCfg, *m_positionMgr,
                                                  *m_drawdownGuard,
                                                  *m_rateLimiter);

        m_spotPlacer = std::make_unique<FakeOrderPlacer>();
        m_placer = std::make_unique<FakeOrderPlacer>();
        m_cfdPlacer = std::make_unique<FakeOrderPlacer>();
        m_flow = std::make_unique<OrderFlowExecutor>(
            m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
            m_spotPlacer.get(), m_placer.get(), m_cfdPlacer.get(),
            nullptr, m_tracker.get(), m_cfdTracker.get(),
            nullptr, nullptr, m_restMutex, nullptr);
    }

    void completeOrder(const std::string &order_id, Side side,
                       double qty, double price,
                       const std::string &client_id = "")
    {
        // Simulate the WS fill event through the tracker, which fires the
        // completion callback (same path as live fills).
        m_tracker->testSimulateWsUpdate(nlohmann::json{
            { "id", order_id },
            { "status", "closed" },
            { "filled_total", std::to_string(qty) },
            { "avg_deal_price", std::to_string(price) },
            { "fee", "0.1" },
        });
    }

    /// Simulate a CFD list-poll fill (state=2/finished=1 + the fill fields
    /// pollCfdOrderStatus synthesizes from the positions fallback) through
    /// the CFD tracker — same completion-callback path as live fills.
    void completeCfdOrder(const std::string &order_id, double qty, double price)
    {
        m_cfdTracker->testSimulateCfdPoll(nlohmann::json{
            { "order_id", order_id },
            { "state", 2 },
            { "finished", 1 },
            { "filled_volume", std::to_string(qty) },
            { "fill_price", std::to_string(price) },
            { "fee", "-0.06" },
        });
    }

    /// Simulate a cancelled (or partially-filled-then-cancelled) order report.
    void completeCancelled(const std::string &order_id, double filled_qty = 0.0,
                           double price = 0.0)
    {
        m_tracker->testSimulateWsUpdate(nlohmann::json{
            { "id", order_id },
            { "status", "finished" },
            { "finish_as", "cancelled" },
            { "filled_total", std::to_string(filled_qty) },
            { "avg_deal_price", std::to_string(price) },
            { "fee", "0" },
        });
    }

    /// Simulate a non-terminal WS update carrying a partial fill.
    void partialFill(const std::string &order_id, double filled_qty)
    {
        m_tracker->testSimulateWsUpdate(nlohmann::json{
            { "id", order_id },
            { "status", "open" },
            { "finish_as", "open" },
            { "filled_total", std::to_string(filled_qty) },
            { "avg_deal_price", "66000" },
            { "fee", "0" },
        });
    }

    /// Seed a one-level order book snapshot for BTC_USDT.
    void applyBook(double bid, double ask)
    {
        m_book.applySnapshot("BTC_USDT", nlohmann::json{
            { "lastUpdateId", 1 },
            { "bids", nlohmann::json::array({ { bid, 5.0 } }) },
            { "asks", nlohmann::json::array({ { ask, 5.0 } }) } });
    }

    /// Rebuild the flow with the fixture's order book wired in.
    void rebuildWithBook()
    {
        m_flow = std::make_unique<OrderFlowExecutor>(
            m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
            nullptr, m_placer.get(), nullptr,
            nullptr, m_tracker.get(), nullptr,
            &m_book, &m_book, m_restMutex, nullptr);
    }

    /// Push a BTC_USDT futures instance with the given order config.
    void addFuturesInstance(OrderType order_type, std::uint32_t timeout_ms = 0)
    {
        StrategyInstanceConfig inst;
        inst.name = "momentum_scalper";
        inst.symbol = "BTC_USDT";
        inst.market_type = MarketType::Futures;
        inst.order_quantity = 2.0;
        inst.leverage = 10.0;
        inst.enabled = true;
        inst.order_type = order_type;
        inst.maker_timeout_ms = timeout_ms;
        m_strategyCfg.strategies.push_back(inst);
    }

    /// Build a standard BTC_USDT futures buy signal.
    TradingSignal makeBuySignal() const
    {
        TradingSignal sig;
        sig.type = SignalType::Buy;
        sig.symbol = "BTC_USDT";
        sig.price = 65000.0;
        sig.strategy_id = "signal_aggregator";   // won't match → fallback path
        sig.market_type = MarketType::Futures;
        sig.confidence = 0.9;
        return sig;
    }

    /// Rebuild the risk stack with tight 500/300 limits (trading.toml values)
    /// so Modified (capped) orders are exercised.
    void rebuildTightStack()
    {
        m_riskCfg.maxPositionNotional = 500.0;
        m_riskCfg.maxSymbolNotional = 300.0;
        m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
        m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
        m_rateLimiter = std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
        m_riskMgr = std::make_unique<RiskManager>(m_riskCfg, *m_positionMgr,
                                                  *m_drawdownGuard, *m_rateLimiter);
        m_flow = std::make_unique<OrderFlowExecutor>(
            m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
            nullptr, m_placer.get(), nullptr,
            nullptr, m_tracker.get(), nullptr,
            nullptr, nullptr, m_restMutex, nullptr);
    }

    ExchangeConfig m_config;
    // Generous limits: openPosition() re-validates notional on fill, so a
    // 1-contract BTC order (~65k notional) must fit within the budget.
    RiskConfig m_riskCfg = []()
    {
        RiskConfig cfg;
        cfg.maxPositionNotional = 1'000'000.0;
        cfg.maxSymbolNotional = 1'000'000.0;
        cfg.maxOpenPositions = 3;
        return cfg;
    }();
    StrategyConfig m_strategyCfg;

    std::unique_ptr<GateWsClient> m_wsClient;
    std::unique_ptr<GateRestClient> m_restClient;
    std::unique_ptr<OrderTracker> m_tracker;
    std::unique_ptr<OrderTracker> m_cfdTracker;
    std::unique_ptr<PositionManager> m_positionMgr;
    std::unique_ptr<DrawdownGuard> m_drawdownGuard;
    std::unique_ptr<OrderRateLimiter> m_rateLimiter;
    std::unique_ptr<RiskManager> m_riskMgr;
    std::unique_ptr<FakeOrderPlacer> m_spotPlacer;
    std::unique_ptr<FakeOrderPlacer> m_placer;
    std::unique_ptr<FakeOrderPlacer> m_cfdPlacer;
    std::unique_ptr<OrderFlowExecutor> m_flow;
    pulse::market::OrderBookManager m_book;
    std::mutex m_restMutex;
};

} // anonymous namespace

TEST_F(OrderFlowTest, PlaceOrderApprovedAndTracked)
{
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;
    req.contract_size = 1;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result));
    EXPECT_EQ("fake_1", value(result).order_id);
    EXPECT_EQ(1, m_placer->place_count);
    EXPECT_EQ(1, m_tracker->activeOrders().size());
}

TEST_F(OrderFlowTest, RejectedOrderNotPlaced)
{
    // Halted trading → orders rejected before reaching the placer.
    m_drawdownGuard->manualHalt();

    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.price = 65000.0;
    req.market_type = MarketType::Futures;

    auto result = m_flow->placeOrder(req);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ManualHalt, error(result).code);
    EXPECT_EQ(0, m_placer->place_count);
}

TEST_F(OrderFlowTest, BudgetExhaustedOrderRejected)
{
    // A huge first order is Modified down to fit the budget (reservation
    // consumes it entirely); a second order then has no budget left.
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1000.0;
    req.price = 65000.0;   // proposed 6.5e7 → Modified to fit 1e6
    req.market_type = MarketType::Futures;

    auto first = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(first)) << error(first).message;

    auto second = m_flow->placeOrder(req);
    ASSERT_FALSE(ok(second));
    EXPECT_EQ(ErrorCode::PositionLimitHit, error(second).code);
}

TEST_F(OrderFlowTest, ModifiedOrderUsesApprovedQty)
{
    // Default risk limits: maxPositionNotional=1000, maxSymbolNotional=300.
    m_riskCfg.maxPositionNotional = 500.0;
    m_riskCfg.maxSymbolNotional = 300.0;
    m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
    m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
    m_rateLimiter = std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
    m_riskMgr = std::make_unique<RiskManager>(m_riskCfg, *m_positionMgr,
                                              *m_drawdownGuard, *m_rateLimiter);
    m_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, m_placer.get(), nullptr,
        nullptr, m_tracker.get(), nullptr,
        nullptr, nullptr, m_restMutex, nullptr);

    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;   // 1 × 65000 = 65000 notional → too big
    req.price = 65000.0;
    req.market_type = MarketType::Futures;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;
    ASSERT_EQ(1, m_placer->place_count);
    // Approved quantity was reduced to fit the symbol limit.
    EXPECT_LT(m_placer->placed[0].quantity, 1.0);
    EXPECT_GT(m_placer->placed[0].quantity, 0.0);
}

TEST_F(OrderFlowTest, PlaceFailureCancelsReservation)
{
    m_placer->scripted_result = PulseError{ ErrorCode::ExchangeError, "rejected by gate" };

    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;

    auto result = m_flow->placeOrder(req);
    ASSERT_FALSE(ok(result));
    // Reservation must not leak — a subsequent order should still fit.
    OrderRequest req2 = req;
    auto result2 = m_flow->placeOrder(req2);
    ASSERT_TRUE(ok(result2)) << error(result2).message;
}

TEST_F(OrderFlowTest, NoExecutorForMarketTypeFails)
{
    // A flow constructed without placers must abort the order instead of
    // dereferencing a null placer. The spot direction must be active first —
    // otherwise the direction gate rejects the order before the placer check.
    auto no_placer_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, nullptr, nullptr,
        nullptr, m_tracker.get(), nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    no_placer_flow->setActiveMarket(MarketType::Spot);

    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Spot;

    auto result = no_placer_flow->placeOrder(req);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::InternalError, error(result).code);
}

TEST_F(OrderFlowTest, FillOpensPositionAndConsumesReservation)
{
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;
    req.contract_size = 1;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result));
    const auto order_id = value(result).order_id;

    // Fill via WS.
    m_tracker->testSimulateWsUpdate(nlohmann::json{
        { "id", order_id },
        { "status", "closed" },
        { "filled_total", "1" },
        { "avg_deal_price", "65000" },
        { "fee", "0.1" },
    });

    EXPECT_EQ(1, m_positionMgr->openPositionCount());
    const auto positions = m_positionMgr->getAllPositions();
    ASSERT_EQ(1, positions.size());
    EXPECT_EQ("BTC_USDT", positions[0].symbol);
    EXPECT_EQ(Side::Buy, positions[0].side);

    // Reservation consumed: another order can still be placed.
    OrderRequest req2 = req;
    auto result2 = m_flow->placeOrder(req2);
    ASSERT_TRUE(ok(result2)) << error(result2).message;
}

TEST_F(OrderFlowTest, SellFillClosesPositionWithPnl)
{
    // Open a position directly, then close it via a sell fill.
    const auto open_result = m_positionMgr->openPosition(
        "BTC_USDT", Side::Buy, 1.0, 60000.0,
        "test_strategy", MarketType::Futures,
        10.0, MarginMode::Cross, 0.001, 0.005);
    ASSERT_TRUE(ok(open_result));

    // Build a real ExecutionReport via the tracker path: place a sell,
    // then simulate its fill.
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result));
    const auto order_id = value(result).order_id;

    m_tracker->testSimulateWsUpdate(nlohmann::json{
        { "id", order_id },
        { "status", "closed" },
        { "filled_total", "1" },
        { "avg_deal_price", "61000" },
        { "fee", "0.1" },
    });

    EXPECT_EQ(0, m_positionMgr->openPositionCount());
}

TEST_F(OrderFlowTest, OnSignalSkipsFlat)
{
    TradingSignal sig;
    sig.type = SignalType::Flat;
    m_flow->onSignal(sig);
    EXPECT_EQ(0, m_placer->place_count);
}

TEST_F(OrderFlowTest, OnSignalUsesStrategyConfigQuantity)
{
    StrategyInstanceConfig inst;
    inst.name = "momentum_scalper";
    inst.symbol = "BTC_USDT";
    inst.market_type = MarketType::Futures;
    inst.order_quantity = 2.0;
    inst.leverage = 10.0;
    inst.enabled = true;
    m_strategyCfg.strategies.push_back(inst);

    m_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, m_placer.get(), nullptr,
        nullptr, m_tracker.get(), nullptr,
        nullptr, nullptr, m_restMutex, nullptr);

    TradingSignal sig;
    sig.type = SignalType::Buy;
    sig.symbol = "BTC_USDT";
    sig.price = 65000.0;
    sig.strategy_id = "signal_aggregator"; // won't match → fallback path
    sig.market_type = MarketType::Futures;
    sig.confidence = 0.9;

    m_flow->onSignal(sig);
    ASSERT_EQ(1, m_placer->place_count);
    // Fallback: first enabled strategy on the symbol → qty 2.0
    EXPECT_DOUBLE_EQ(2.0, m_placer->placed[0].quantity);
}

TEST_F(OrderFlowTest, CancelOrderProbesTracker)
{
    // Tracked order can be cancelled.
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result));

    EXPECT_TRUE(m_flow->cancelOrder(value(result).order_id));
    EXPECT_EQ(1, m_placer->cancel_count);

    // Untracked order id → false.
    EXPECT_FALSE(m_flow->cancelOrder("not_tracked"));
    EXPECT_EQ(1, m_placer->cancel_count);
}

TEST_F(OrderFlowTest, FuturesOrderSetsLeverageBeforePlace)
{
    // Futures orders must apply the requested leverage BEFORE placement —
    // Gate.io sets leverage at the position level, not per order, so the
    // account's current setting would silently apply otherwise.
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;
    req.contract_size = 1;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;

    EXPECT_EQ(1, m_placer->leverage_count);
    EXPECT_EQ("BTC_USDT", m_placer->last_leverage_contract);
    EXPECT_DOUBLE_EQ(10.0, m_placer->last_leverage_value);
    EXPECT_EQ(1, m_placer->place_count); // order still placed after leverage OK
}

TEST_F(OrderFlowTest, FuturesOrderSkipsLeverageWhenUnset)
{
    // leverage == 0 (default) means "don't manage leverage" — no API call.
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;
    // req.leverage left at default 0.0

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;

    EXPECT_EQ(0, m_placer->leverage_count);
    EXPECT_EQ(1, m_placer->place_count);
}

TEST_F(OrderFlowTest, SpotOrderSkipsLeverage)
{
    // Leverage is futures-only — spot orders never touch the leverage API,
    // even when a leverage value is present on the request.
    // Activate the spot direction first (default is futures).
    m_flow->setActiveMarket(MarketType::Spot);
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 0.001;
    req.market_type = MarketType::Spot;
    req.leverage = 10.0;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;

    EXPECT_EQ(0, m_spotPlacer->leverage_count);
    EXPECT_EQ(1, m_spotPlacer->place_count);
    EXPECT_EQ(0, m_placer->leverage_count);
}

TEST_F(OrderFlowTest, LeverageFailureAbortsOrder)
{
    // If the leverage API call fails, the order must NOT be placed — trading
    // at the account's stale leverage (e.g. 200x) would violate the strategy
    // config silently.
    m_placer->fail_leverage = true;

    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;
    req.contract_size = 1;

    auto result = m_flow->placeOrder(req);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::HttpError, error(result).code);
    EXPECT_EQ(0, m_placer->place_count); // aborted before placement
    EXPECT_TRUE(m_tracker->activeOrders().empty());
}

// ---------------------------------------------------------------------------
// Regression tests: single-evaluation flow + futures contract multipliers
// ---------------------------------------------------------------------------

// A signal whose quantity exceeds the notional budget is Modified (capped)
// by risk evaluation; the order must still be PLACED with the reduced
// quantity. Before the fix, placeOrder() re-evaluated the reduced order and
// rejected it against its own reservation — every signal ended in the 3002
// "Position notional limit reached" reject loop.
TEST_F(OrderFlowTest, OnSignalModifiedOrderIsPlaced)
{
    rebuildTightStack();

    StrategyInstanceConfig inst;
    inst.name = "momentum_scalper";
    inst.symbol = "BTC_USDT";
    inst.market_type = MarketType::Futures;
    inst.order_quantity = 1.0;
    inst.leverage = 10.0;
    inst.enabled = true;
    m_strategyCfg.strategies.push_back(inst);
    m_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, m_placer.get(), nullptr,
        nullptr, m_tracker.get(), nullptr,
        nullptr, nullptr, m_restMutex, nullptr);

    TradingSignal sig;
    sig.type = SignalType::Sell;
    sig.symbol = "BTC_USDT";
    sig.price = 65000.0;
    sig.strategy_id = "signal_aggregator";
    sig.market_type = MarketType::Futures;
    sig.confidence = 0.9;

    m_flow->onSignal(sig);
    ASSERT_EQ(1, m_placer->place_count) << "Modified signal order must be placed";
    // Placed with the risk-capped quantity (not the full 1.0).
    EXPECT_GT(m_placer->placed[0].quantity, 0.0);
    EXPECT_LT(m_placer->placed[0].quantity, 1.0);
}

// When a Modified order fails to place (exchange error), its reservation must
// be released so subsequent signals can still place. Before the fix the
// leaked reservation permanently exhausted the symbol budget, rejecting every
// later signal with 3002.
TEST_F(OrderFlowTest, OnSignalModifiedFailureReleasesReservation)
{
    rebuildTightStack();

    StrategyInstanceConfig inst;
    inst.name = "momentum_scalper";
    inst.symbol = "BTC_USDT";
    inst.market_type = MarketType::Futures;
    inst.order_quantity = 1.0;
    inst.leverage = 10.0;
    inst.enabled = true;
    m_strategyCfg.strategies.push_back(inst);
    m_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, m_placer.get(), nullptr,
        nullptr, m_tracker.get(), nullptr,
        nullptr, nullptr, m_restMutex, nullptr);

    TradingSignal sig;
    sig.type = SignalType::Buy;
    sig.symbol = "BTC_USDT";
    sig.price = 65000.0;
    sig.strategy_id = "signal_aggregator";
    sig.market_type = MarketType::Futures;
    sig.confidence = 0.9;

    // First signal: exchange rejects the placement.
    m_placer->scripted_result = PulseError{ ErrorCode::ExchangeError, "gate down" };
    m_flow->onSignal(sig);
    ASSERT_EQ(1, m_placer->place_count);

    // Second signal: the released budget must allow placement.
    m_flow->onSignal(sig);
    ASSERT_EQ(2, m_placer->place_count) << "reservation must be released after failure";
}

// With contract metadata injected, a 1-contract futures order is evaluated at
// its true notional (1 contract * price * 0.0001 = ~6.5 USDT for BTC_USDT)
// and placed at the FULL quantity. Before the fix, 1 contract was treated as
// 1 BTC (~65k USDT), capped to a sub-contract size, and the exchange would
// reject the invalid size.
TEST_F(OrderFlowTest, FuturesQuantoKeepsFullContractQuantity)
{
    rebuildTightStack();

    auto registry = std::make_shared<market::SymbolRegistry>(
        *m_restClient, MarketType::Futures);
    market::SymbolInfo info;
    info.symbol = "BTC_USDT";
    info.market_type = MarketType::Futures;
    info.quanto_multiplier = 0.0001;
    registry->upsert(info);
    m_flow->setSymbolRegistry(registry);

    StrategyInstanceConfig inst;
    inst.name = "momentum_scalper";
    inst.symbol = "BTC_USDT";
    inst.market_type = MarketType::Futures;
    inst.order_quantity = 1.0;
    inst.leverage = 10.0;
    inst.enabled = true;
    m_strategyCfg.strategies.push_back(inst);
    m_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, m_placer.get(), nullptr,
        nullptr, m_tracker.get(), nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    m_flow->setSymbolRegistry(registry);

    TradingSignal sig;
    sig.type = SignalType::Buy;
    sig.symbol = "BTC_USDT";
    sig.price = 65000.0;
    sig.strategy_id = "signal_aggregator";
    sig.market_type = MarketType::Futures;
    sig.confidence = 0.9;

    m_flow->onSignal(sig);
    ASSERT_EQ(1, m_placer->place_count);
    EXPECT_DOUBLE_EQ(1.0, m_placer->placed[0].quantity)
        << "1 contract fits within the budget at true notional";
}

// A SELL fill with no matching long must OPEN a short position. Before the
// fix, sells only closed longs — shorts were never recorded, leaving the risk
// gate blind to real short exposure.
TEST_F(OrderFlowTest, SellFillOpensShortWhenNoLong)
{
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;
    ASSERT_EQ(1, m_placer->place_count);

    completeOrder("fake_1", Side::Sell, 1.0, 65000.0);

    const auto positions = m_positionMgr->getPositionsBySymbol("BTC_USDT");
    ASSERT_EQ(1, positions.size());
    EXPECT_EQ(Side::Sell, positions[0].side);
    EXPECT_DOUBLE_EQ(1.0, positions[0].quantity);
    EXPECT_EQ(MarketType::Futures, positions[0].market_type);
    EXPECT_DOUBLE_EQ(10.0, positions[0].leverage);
}

// ---------------------------------------------------------------------------
// Direction gate tests (M15 — single active trading direction)
// ---------------------------------------------------------------------------

TEST_F(OrderFlowTest, GateRejectsInactiveMarketOrder)
{
    // Default active market is futures — a CFD order must be rejected at the
    // gate without touching the CFD placer.
    OrderRequest req;
    req.symbol = "XAUUSD";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 0.01;
    req.market_type = MarketType::Cfd;
    req.leverage = 500.0;
    req.quanto_multiplier = 100.0;

    auto result = m_flow->placeOrder(req);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::InactiveMarket, error(result).code);
    EXPECT_EQ(0, m_cfdPlacer->place_count);
}

TEST_F(OrderFlowTest, SwitchActiveMarketAllowsCfdAndRejectsFutures)
{
    m_flow->setActiveMarket(MarketType::Cfd);

    OrderRequest cfd_req;
    cfd_req.symbol = "XAUUSD";
    cfd_req.side = Side::Buy;
    cfd_req.type = OrderType::Market;
    cfd_req.quantity = 0.01;
    cfd_req.market_type = MarketType::Cfd;
    cfd_req.leverage = 500.0;
    cfd_req.quanto_multiplier = 100.0;

    auto result = m_flow->placeOrder(cfd_req);
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_cfdPlacer->place_count);
    EXPECT_EQ(0, m_placer->place_count);

    // Futures now inactive — rejected at the gate.
    OrderRequest futures_req;
    futures_req.symbol = "BTC_USDT";
    futures_req.side = Side::Buy;
    futures_req.type = OrderType::Market;
    futures_req.quantity = 1.0;
    futures_req.market_type = MarketType::Futures;
    futures_req.leverage = 10.0;

    result = m_flow->placeOrder(futures_req);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::InactiveMarket, error(result).code);
    EXPECT_EQ(0, m_placer->place_count); // futures placer never touched
}

TEST_F(OrderFlowTest, ReduceOnlyExemptFromDirectionGate)
{
    // After switching to CFD, closing an old futures position (reduce_only)
    // must still pass the gate.
    m_flow->setActiveMarket(MarketType::Cfd);

    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Sell;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;
    req.reduce_only = true;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_placer->place_count);
}

TEST_F(OrderFlowTest, SignalFromInactiveMarketIsSkipped)
{
    // A signal for the inactive direction must be dropped without consuming
    // rate-limiter tokens or reaching the placer.
    pulse::strategy::TradingSignal sig;
    sig.symbol = "XAUUSD";
    sig.market_type = MarketType::Cfd;
    sig.type = pulse::strategy::SignalType::Buy;
    sig.price = 4348.0;
    sig.confidence = 0.9;
    sig.strategy_id = "momentum_scalper";
    sig.reason = "test";

    m_flow->onSignal(sig);
    EXPECT_EQ(0, m_cfdPlacer->place_count);
    EXPECT_EQ(0, m_placer->place_count);
}

TEST_F(OrderFlowTest, CancelAllOpenOrdersSweepsMarket)
{
    // Track a futures order, then sweep — the cancel must reach the placer.
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.quantity = 1.0;
    req.price = 60000.0;
    req.market_type = MarketType::Futures;
    req.leverage = 10.0;

    auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;
    ASSERT_EQ(1, m_placer->place_count);

    m_tracker->trackOrder("fake_1", "BTC_USDT", Side::Buy, OrderType::Limit,
                          1.0, 60000.0);

    std::lock_guard lock(m_restMutex);
    const int cancelled = m_flow->cancelAllOpenOrders(MarketType::Futures);
    EXPECT_GE(cancelled, 1);
    EXPECT_GE(m_placer->cancel_count, 1);
}

// ---------------------------------------------------------------------------
// Maker-first order flow (order_type = post_only / maker_first)
// ---------------------------------------------------------------------------

TEST_F(OrderFlowTest, SignalWithDefaultMarketTypePlacesMarketOrder)
{
    // Regression guard: no order_type config → unchanged market-order behavior.
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);
    EXPECT_EQ(OrderType::Market, m_placer->placed[0].type);
    EXPECT_DOUBLE_EQ(65000.0, m_placer->placed[0].price);
}

TEST_F(OrderFlowTest, SignalPostOnlyBuyUsesBestBid)
{
    addFuturesInstance(OrderType::PostOnly);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);
    EXPECT_EQ(OrderType::PostOnly, m_placer->placed[0].type);
    EXPECT_DOUBLE_EQ(66000.0, m_placer->placed[0].price);
}

TEST_F(OrderFlowTest, SignalPostOnlySellUsesBestAsk)
{
    addFuturesInstance(OrderType::PostOnly);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    auto sig = makeBuySignal();
    sig.type = SignalType::Sell;   // sell signal → price at best ASK

    m_flow->onSignal(sig);
    ASSERT_EQ(1, m_placer->place_count);
    EXPECT_EQ(OrderType::PostOnly, m_placer->placed[0].type);
    EXPECT_DOUBLE_EQ(66010.0, m_placer->placed[0].price);
}

TEST_F(OrderFlowTest, SignalMakerFirstWithoutBookFallsBackToMarket)
{
    addFuturesInstance(OrderType::MakerFirst, 500);
    rebuildWithBook();   // empty book — no snapshot applied

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);
    EXPECT_EQ(OrderType::Market, m_placer->placed[0].type);

    // No post-only order was placed → no attempt, sweep is a no-op.
    m_flow->sweepMakerAttempts();
    EXPECT_EQ(1, m_placer->place_count);
    EXPECT_EQ(0, m_placer->cancel_count);
}

TEST_F(OrderFlowTest, SignalPostOnlyWithoutBookDropsSignal)
{
    addFuturesInstance(OrderType::PostOnly);
    rebuildWithBook();   // empty book

    m_flow->onSignal(makeBuySignal());
    EXPECT_EQ(0, m_placer->place_count);   // post_only never crosses the spread
}

TEST_F(OrderFlowTest, MakerFirstExpiredAttemptCancelsAndFallsBack)
{
    addFuturesInstance(OrderType::MakerFirst, 1);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);
    EXPECT_EQ(OrderType::PostOnly, m_placer->placed[0].type);
    EXPECT_DOUBLE_EQ(66000.0, m_placer->placed[0].price);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m_flow->sweepMakerAttempts();

    // Maker order cancelled, market fallback placed for the full remainder.
    ASSERT_EQ(1, m_placer->cancel_count);
    ASSERT_EQ(2, m_placer->place_count);
    EXPECT_EQ(OrderType::Market, m_placer->placed[1].type);
    EXPECT_DOUBLE_EQ(2.0, m_placer->placed[1].quantity);
    EXPECT_EQ(Side::Buy, m_placer->placed[1].side);

    // Cancelled maker report (filled 0) opens nothing; fallback fill opens 1.
    completeCancelled("fake_1");
    completeOrder("fake_2", Side::Buy, 2.0, 66000.0);
    EXPECT_EQ(1, m_positionMgr->openPositionCount());
}

TEST_F(OrderFlowTest, MakerFirstNotExpiredNoFallback)
{
    addFuturesInstance(OrderType::MakerFirst, 60000);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);

    m_flow->sweepMakerAttempts();   // before the 60s deadline
    EXPECT_EQ(0, m_placer->cancel_count);
    EXPECT_EQ(1, m_placer->place_count);
}

TEST_F(OrderFlowTest, MakerFirstCancelRejectedLeavesAttempt)
{
    addFuturesInstance(OrderType::MakerFirst, 1);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);

    // Exchange refuses the cancel (order already filled / rejected) → no
    // fallback; the attempt survives for the next sweep.
    m_placer->fail_cancel = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m_flow->sweepMakerAttempts();
    EXPECT_EQ(1, m_placer->cancel_count);
    EXPECT_EQ(1, m_placer->place_count);

    // Terminal report arrives → attempt cleaned up; next sweep is a no-op.
    m_placer->fail_cancel = false;
    completeCancelled("fake_1");
    m_flow->sweepMakerAttempts();
    EXPECT_EQ(1, m_placer->cancel_count);
    EXPECT_EQ(1, m_placer->place_count);
}

TEST_F(OrderFlowTest, ExchangeRejectedPostOnlyNoMarketFallback)
{
    addFuturesInstance(OrderType::MakerFirst, 1);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    // Exchange rejects the post-only outright (would have crossed).
    m_placer->scripted_result =
        PulseError{ ErrorCode::OrderRejected, "post-only would match" };

    m_flow->onSignal(makeBuySignal());
    EXPECT_EQ(1, m_placer->place_count);   // placement attempted, failed
    EXPECT_EQ(0, m_placer->cancel_count);

    // No attempt was registered (placement failed) → no chase with a taker.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m_flow->sweepMakerAttempts();
    EXPECT_EQ(1, m_placer->place_count);
    EXPECT_EQ(0, m_placer->cancel_count);
}

TEST_F(OrderFlowTest, CancelledReportBeforeExpiryErasesAttempt)
{
    addFuturesInstance(OrderType::MakerFirst, 1);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);

    // Exchange-side cancel (rejected post-only) arrives before the deadline.
    completeCancelled("fake_1");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m_flow->sweepMakerAttempts();

    EXPECT_EQ(0, m_placer->cancel_count);
    EXPECT_EQ(1, m_placer->place_count);
}

TEST_F(OrderFlowTest, PartialFillFallbackUsesRemainingQuantity)
{
    addFuturesInstance(OrderType::MakerFirst, 1);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);

    partialFill("fake_1", 0.8);   // 0.8 of 2.0 filled, order still open
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m_flow->sweepMakerAttempts();

    ASSERT_EQ(1, m_placer->cancel_count);
    ASSERT_EQ(2, m_placer->place_count);
    EXPECT_EQ(OrderType::Market, m_placer->placed[1].type);
    EXPECT_NEAR(1.2, m_placer->placed[1].quantity, 1e-9);   // remainder only

    // Cancelled report for the partially-filled maker order: the 0.8 must
    // open with the original futures metadata (kept via reservation entry).
    completeCancelled("fake_1", 0.8, 66000.0);
    auto positions = m_positionMgr->getPositionsBySymbol("BTC_USDT");
    ASSERT_EQ(1u, positions.size());
    EXPECT_EQ(MarketType::Futures, positions[0].market_type);
    EXPECT_DOUBLE_EQ(10.0, positions[0].leverage);
    EXPECT_NEAR(0.8, positions[0].quantity, 1e-9);

    // Fallback fill completes the intended total quantity.
    completeOrder("fake_2", Side::Buy, 1.2, 66000.0);
    positions = m_positionMgr->getPositionsBySymbol("BTC_USDT");
    double total_qty = 0.0;
    for (const auto &pos : positions)
    {
        total_qty += pos.quantity;
    }
    EXPECT_NEAR(2.0, total_qty, 1e-9);
}

TEST_F(OrderFlowTest, FallbackRejectedByRateLimit)
{
    addFuturesInstance(OrderType::MakerFirst, 1);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());   // consumes 1 rate-limiter token
    ASSERT_EQ(1, m_placer->place_count);

    // Drain the token bucket so the fallback's fresh evaluation is rejected.
    while (m_rateLimiter->tryAcquire())
    {
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m_flow->sweepMakerAttempts();

    EXPECT_EQ(1, m_placer->cancel_count);
    EXPECT_EQ(1, m_placer->place_count);   // fallback rejected — no trade
    EXPECT_EQ(0, m_positionMgr->openPositionCount());
}

TEST_F(OrderFlowTest, FallbackBlockedAfterDirectionSwitch)
{
    addFuturesInstance(OrderType::MakerFirst, 1);
    applyBook(66000.0, 66010.0);
    rebuildWithBook();

    m_flow->onSignal(makeBuySignal());
    ASSERT_EQ(1, m_placer->place_count);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    m_flow->setActiveMarket(MarketType::Spot);   // switch away mid-attempt

    m_flow->sweepMakerAttempts();
    EXPECT_EQ(1, m_placer->cancel_count);
    // Fallback rejected by the direction gate (InactiveMarket) — switch wins.
    EXPECT_EQ(1, m_placer->place_count);
}

TEST_F(OrderFlowTest, SweepToleratesNullBookAndEmptyAttempts)
{
    // Fixture has no order book and no attempts — sweep must be a safe no-op.
    m_flow->sweepMakerAttempts();
    EXPECT_EQ(0, m_placer->place_count);
    EXPECT_EQ(0, m_placer->cancel_count);
}

// ---------------------------------------------------------------------------
// M17: per-market notional budget — end-to-end through OrderFlowExecutor
// ---------------------------------------------------------------------------

TEST_F(OrderFlowTest, PerMarketBudget_CfdOrderNotClampedByFuturesPosition)
{
    // Regression for the 2026-08-17 incident: with a SKHY futures position
    // occupying 5099 of the FUTURES cap, a 0.01-lot CFD order (4392 USDT
    // notional) must pass the risk gate at full quantity under the CFD cap.
    // (Old shared-budget code clamped it to ~0.003 lots → exchange reject.)
    m_riskCfg.maxPositionNotional = 1000.0;       // fallback (not used here)
    m_riskCfg.maxPositionNotionalFutures = 6000.0;
    m_riskCfg.maxPositionNotionalCfd = 6000.0;
    m_riskCfg.maxSymbolNotional = 6000.0;
    m_riskCfg.maxOpenPositions = 5;

    // Rebuild the stack AFTER setting caps (PositionManager copies the config)
    // with the CFD placer/tracker wired in — same shape as SetUp.
    m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
    m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
    m_rateLimiter = std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
    m_riskMgr = std::make_unique<RiskManager>(m_riskCfg, *m_positionMgr,
                                              *m_drawdownGuard, *m_rateLimiter);
    m_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_spotPlacer.get(), m_placer.get(), m_cfdPlacer.get(),
        nullptr, m_tracker.get(), m_cfdTracker.get(),
        nullptr, nullptr, m_restMutex, nullptr);

    // SKHY futures short: 3000 contracts * 169.97 * 0.01 = 5099.1 notional.
    const auto skhy = m_positionMgr->openPosition(
        "SKHY_USDT", Side::Sell, 3000.0, 169.97, "manual",
        MarketType::Futures, 25.0, MarginMode::Cross, 0.01, 0.025);
    ASSERT_TRUE(ok(skhy));

    // CFD must be the active direction for the gate to pass.
    m_flow->setActiveMarket(MarketType::Cfd);

    OrderRequest req;
    req.symbol = "XAUUSD";
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.quantity = 0.01;
    req.price = 4392.0;
    req.market_type = MarketType::Cfd;
    req.quanto_multiplier = 100.0;

    const auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_cfdPlacer->place_count);
    ASSERT_EQ(1u, m_cfdPlacer->placed.size());
    EXPECT_DOUBLE_EQ(0.01, m_cfdPlacer->placed[0].quantity);
    // The futures placer must not see the CFD order.
    EXPECT_EQ(0, m_placer->place_count);
}

TEST_F(OrderFlowTest, CfdFillOpensPositionWithCfdMetadata)
{
    // M19: a CFD market-order fill (detected via the positions fallback in
    // pollCfdOrderStatus) must flow through the completion callback into
    // PositionManager with the CFD market metadata — the engine's CFD fills
    // were invisible before (no tracked position, no report, no recordTrade).
    m_riskCfg.maxPositionNotional = 6000.0;   // fallback cap
    m_riskCfg.maxPositionNotionalCfd = 6000.0;
    m_riskCfg.maxSymbolNotional = 6000.0;
    m_riskCfg.maxOpenPositions = 5;

    // Rebuild the stack AFTER setting caps (PositionManager copies the
    // config) — same shape as SetUp.
    m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
    m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
    m_rateLimiter = std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
    m_riskMgr = std::make_unique<RiskManager>(m_riskCfg, *m_positionMgr,
                                              *m_drawdownGuard, *m_rateLimiter);
    m_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_spotPlacer.get(), m_placer.get(), m_cfdPlacer.get(),
        nullptr, m_tracker.get(), m_cfdTracker.get(),
        nullptr, nullptr, m_restMutex, nullptr);

    m_flow->setActiveMarket(MarketType::Cfd);

    OrderRequest req;
    req.symbol = "XAUUSD";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 0.01;
    req.market_type = MarketType::Cfd;
    req.leverage = 500.0;
    req.quanto_multiplier = 100.0;

    const auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_cfdPlacer->place_count);
    const std::string order_id = value(result).order_id;
    ASSERT_FALSE(order_id.empty());

    // The immediate poll (CFD market orders) hits the network-less client,
    // fails gracefully and leaves the order tracked — then the fill arrives
    // via the CFD tracker state machine (positions-fallback shape).
    completeCfdOrder(order_id, 0.01, 4406.68);

    // The fill must open a tracked XAUUSD long with CFD metadata.
    EXPECT_EQ(1, m_positionMgr->openPositionCount());
    const auto positions = m_positionMgr->getPositionsBySymbol("XAUUSD");
    ASSERT_EQ(1u, positions.size());
    const auto &pos = positions[0];
    EXPECT_EQ(Side::Buy, pos.side);
    EXPECT_DOUBLE_EQ(0.01, pos.quantity);
    EXPECT_EQ(MarketType::Cfd, pos.market_type);
    EXPECT_DOUBLE_EQ(500.0, pos.leverage);
    EXPECT_DOUBLE_EQ(4406.68, pos.entry_price);
}

TEST_F(OrderFlowTest, CfdMarketOrderTriggersImmediatePollNoCrash)
{
    // Regression guard: the immediate poll for CFD market orders runs against
    // a network-less REST client — it must fail with a WARN and leave the
    // order tracked, never crash or open a phantom position.
    m_riskCfg.maxPositionNotionalCfd = 6000.0;
    m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
    m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
    m_rateLimiter = std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
    m_riskMgr = std::make_unique<RiskManager>(m_riskCfg, *m_positionMgr,
                                              *m_drawdownGuard, *m_rateLimiter);
    m_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_spotPlacer.get(), m_placer.get(), m_cfdPlacer.get(),
        nullptr, m_tracker.get(), m_cfdTracker.get(),
        nullptr, nullptr, m_restMutex, nullptr);

    m_flow->setActiveMarket(MarketType::Cfd);

    OrderRequest req;
    req.symbol = "XAUUSD";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 0.01;
    req.market_type = MarketType::Cfd;
    req.leverage = 500.0;
    req.quanto_multiplier = 100.0;

    const auto result = m_flow->placeOrder(req);
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_cfdPlacer->place_count);

    // No fill was simulated — no position, order still tracked (trackOrder
    // starts at Pending; the failed poll leaves it there).
    EXPECT_EQ(0, m_positionMgr->openPositionCount());
    const auto active = m_cfdTracker->activeOrders();
    ASSERT_EQ(1u, active.size());
    EXPECT_EQ(OrderStatus::Pending, active[0].status);
}
