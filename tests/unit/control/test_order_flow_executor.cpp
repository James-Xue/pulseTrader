// test_order_flow_executor.cpp — Unit tests for OrderFlowExecutor
//
// Uses a FakeOrderPlacer so no network is involved; OrderTracker is
// driven via its testSimulateWsUpdate() interface.

#include "control/OrderFlowExecutor.hpp"

#include "execution/OrderTracker.hpp"
#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"
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
        return true;
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
        m_tracker = std::make_unique<OrderTracker>(*m_wsClient, *m_restClient,
                                                   MarketType::Futures);
        m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
        m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
        m_rateLimiter = std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
        m_riskMgr = std::make_unique<RiskManager>(m_riskCfg, *m_positionMgr,
                                                  *m_drawdownGuard,
                                                  *m_rateLimiter);

        m_spotPlacer = std::make_unique<FakeOrderPlacer>();
        m_placer = std::make_unique<FakeOrderPlacer>();
        m_flow = std::make_unique<OrderFlowExecutor>(
            m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
            m_spotPlacer.get(), m_placer.get(), nullptr, m_tracker.get(), m_restMutex, nullptr);
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
            nullptr, m_placer.get(), nullptr, m_tracker.get(), m_restMutex, nullptr);
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
    std::unique_ptr<PositionManager> m_positionMgr;
    std::unique_ptr<DrawdownGuard> m_drawdownGuard;
    std::unique_ptr<OrderRateLimiter> m_rateLimiter;
    std::unique_ptr<RiskManager> m_riskMgr;
    std::unique_ptr<FakeOrderPlacer> m_spotPlacer;
    std::unique_ptr<FakeOrderPlacer> m_placer;
    std::unique_ptr<OrderFlowExecutor> m_flow;
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
        nullptr, m_placer.get(), nullptr, m_tracker.get(), m_restMutex, nullptr);

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
    // dereferencing a null placer.
    auto no_placer_flow = std::make_unique<OrderFlowExecutor>(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, nullptr, nullptr, m_tracker.get(), m_restMutex, nullptr);

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
        nullptr, m_placer.get(), nullptr, m_tracker.get(), m_restMutex, nullptr);

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
        nullptr, m_placer.get(), nullptr, m_tracker.get(), m_restMutex, nullptr);

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
        nullptr, m_placer.get(), nullptr, m_tracker.get(), m_restMutex, nullptr);

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
        nullptr, m_placer.get(), nullptr, m_tracker.get(), m_restMutex, nullptr);
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
