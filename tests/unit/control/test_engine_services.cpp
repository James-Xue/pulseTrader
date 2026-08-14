// test_engine_services.cpp — Unit tests for EngineServices (empty-state)

#include "control/EngineServices.hpp"
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
using namespace pulse::exchange;
using namespace pulse::risk;
using namespace pulse::strategy;

namespace
{

class FakePlacer : public IOrderPlacer
{
  public:
    Result<execution::OrderResponse> place(const execution::OrderRequest &) override
    {
        execution::OrderResponse resp;
        resp.order_id = "fake";
        resp.status = OrderStatus::Open;
        return resp;
    }
    bool cancel(const std::string &) override { return true; }
    Result<nlohmann::json> setLeverage(const std::string &, double) override
    {
        return nlohmann::json{ { "ok", true } };
    }
};

} // anonymous namespace

class EngineServicesTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_wsClient = std::make_unique<GateWsClient>(m_config);
        m_restClient = std::make_unique<GateRestClient>(m_config);
        m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
        m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
        m_rateLimiter = std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
        m_riskMgr = std::make_unique<RiskManager>(m_riskCfg, *m_positionMgr,
                                                  *m_drawdownGuard,
                                                  *m_rateLimiter);
        m_placer = std::make_unique<FakePlacer>();
        m_flow = std::make_unique<OrderFlowExecutor>(
            m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
            m_placer.get(), nullptr, nullptr, nullptr, m_restMutex, nullptr);

        m_services = std::make_unique<EngineServices>(
            "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
            nullptr, nullptr, m_restClient.get(), nullptr,
            nullptr, nullptr, *m_flow, m_restMutex);
    }

    ExchangeConfig m_config;
    PulseConfig m_cfg;
    RiskConfig m_riskCfg;
    StrategyConfig m_strategyCfg;
    StrategyManager m_strategyMgr;
    std::chrono::steady_clock::time_point m_start =
        std::chrono::steady_clock::now();

    std::unique_ptr<GateWsClient> m_wsClient;
    std::unique_ptr<GateRestClient> m_restClient;
    std::unique_ptr<PositionManager> m_positionMgr;
    std::unique_ptr<DrawdownGuard> m_drawdownGuard;
    std::unique_ptr<OrderRateLimiter> m_rateLimiter;
    std::unique_ptr<RiskManager> m_riskMgr;
    std::unique_ptr<FakePlacer> m_placer;
    std::unique_ptr<OrderFlowExecutor> m_flow;
    std::unique_ptr<EngineServices> m_services;
    std::mutex m_restMutex;
};

TEST_F(EngineServicesTest, StatusReportsEngineState)
{
    const auto j = m_services->status();
    EXPECT_EQ("test", j["version"]);
    EXPECT_EQ("mainnet", j["network"]);
    EXPECT_EQ(0, j["strategies_total"].get<int>());
    EXPECT_EQ(0, j["open_positions"].get<int>());
    EXPECT_FALSE(j["trading_halted"].get<bool>());
}

TEST_F(EngineServicesTest, PositionsEmptyState)
{
    const auto j = m_services->positions();
    EXPECT_EQ(0, j["positions"].size());
    EXPECT_EQ(0, j["portfolio"]["openPositionCount"].get<int>());
}

TEST_F(EngineServicesTest, StrategiesEmptyState)
{
    EXPECT_TRUE(m_services->strategies().empty());
}

TEST_F(EngineServicesTest, RiskSnapshotExposesHaltState)
{
    const auto j = m_services->risk();
    EXPECT_FALSE(j["trading_halted"].get<bool>());
}

TEST_F(EngineServicesTest, GetParamsUnknownStrategyReturnsNull)
{
    EXPECT_TRUE(m_services->getStrategyParams("no_such_strategy").is_null());
}

TEST_F(EngineServicesTest, SetParamUnknownStrategyFails)
{
    EXPECT_FALSE(m_services->setStrategyParam("no_such", "min_confidence", 0.5));
}

TEST_F(EngineServicesTest, HaltResumeRoundTrip)
{
    m_services->haltTrading();
    EXPECT_TRUE(m_services->risk()["trading_halted"].get<bool>());
    m_services->resumeTrading();
    EXPECT_FALSE(m_services->risk()["trading_halted"].get<bool>());
}

TEST_F(EngineServicesTest, OpenOrderRequiresValidParams)
{
    // Missing symbol → invalid request.
    auto result = m_services->openOrder(nlohmann::json{ { "side", "buy" } });
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);

    // Bad side → invalid request.
    result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "sideways" },
        { "quantity", 1.0 },
    });
    ASSERT_FALSE(ok(result));

    // Valid request → placed via fake placer.
    result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
    });
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("fake", value(result).order_id);
}

TEST_F(EngineServicesTest, ClosePositionUnknownFails)
{
    auto result = m_services->closePosition(
        nlohmann::json{ { "position_id", "nope" } });
    ASSERT_FALSE(ok(result));
}

TEST_F(EngineServicesTest, MarketNoFeedReturnsError)
{
    const auto j = m_services->market("BTC_USDT", 5, 0);
    EXPECT_TRUE(j.contains("error"));
}
