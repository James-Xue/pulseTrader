// test_order_flow_executor.cpp — Unit tests for OrderFlowExecutor
//
// Uses a FakeOrderPlacer so no network is involved; OrderTracker is
// driven via its testSimulateWsUpdate() interface.

#include "control/OrderFlowExecutor.hpp"

#include "execution/OrderTracker.hpp"
#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"
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

        m_placer = std::make_unique<FakeOrderPlacer>();
        m_flow = std::make_unique<OrderFlowExecutor>(
            m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
            nullptr, m_placer.get(), nullptr, m_tracker.get(), m_restMutex, nullptr);
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
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Market;
    req.quantity = 1.0;
    req.market_type = MarketType::Spot; // spot placer is nullptr in fixture

    auto result = m_flow->placeOrder(req);
    ASSERT_FALSE(ok(result));
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
