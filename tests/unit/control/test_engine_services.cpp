// test_engine_services.cpp — Unit tests for EngineServices (empty-state)

#include "control/EngineServices.hpp"
#include "control/JsonRpcServer.hpp"
#include "control/OrderFlowExecutor.hpp"

#include "core/TimeUtil.hpp"
#include "strategy/signal/SignalBoard.hpp"
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
            m_placer.get(), nullptr, nullptr,
            nullptr, nullptr, nullptr,
            nullptr, nullptr, m_restMutex, nullptr);
        m_board = std::make_unique<SignalBoard>(
            m_strategyCfg.signal_aggregator_threshold);

        m_services = std::make_unique<EngineServices>(
            "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
            nullptr, nullptr, nullptr,
            m_restClient.get(), nullptr, nullptr,
            nullptr, nullptr, nullptr, *m_flow, *m_board, m_restMutex);
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
    std::unique_ptr<SignalBoard> m_board;
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

TEST_F(EngineServicesTest, PositionsIncludeHumanReadableOpenTime)
{
    // A display timezone different from the machine local (US Eastern):
    // the *_str companion must be formatted in that timezone.
    m_cfg.control.displayTimezone = "-04:00";
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        m_restClient.get(), nullptr, nullptr,
        nullptr, nullptr, nullptr, *m_flow, *m_board, m_restMutex);

    // Small qty keeps notional under the fixture's risk limits
    // (per-symbol 500 USDT).
    const auto opened = m_positionMgr->openPosition(
        "BTC_USDT", Side::Sell, 0.007, 63075.5, "s1");
    ASSERT_TRUE(ok(opened));

    const auto j = svc.positions();
    ASSERT_EQ(1, j["positions"].size());
    const auto &p = j["positions"][0];
    ASSERT_TRUE(p.contains("open_time_str"));
    const auto tz = parseDisplayTimezone("-04:00");
    ASSERT_TRUE(tz);
    EXPECT_EQ(p["open_time_str"].get<std::string>(),
              formatEpochMs(p["open_time"].get<std::int64_t>(), *tz));
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

    // Valid request → placed via fake placer. The spot direction must be
    // active (default is futures) or the direction gate rejects the order.
    m_flow->setActiveMarket(MarketType::Spot);
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

// ---------------------------------------------------------------------------
// Direction switch tests (M15)
// ---------------------------------------------------------------------------

TEST_F(EngineServicesTest, StatusReportsActiveMarket)
{
    const auto j = m_services->status();
    EXPECT_EQ("futures", j["active_market"].get<std::string>());
}

TEST_F(EngineServicesTest, SwitchDirectionToUnconfiguredCfdFails)
{
    // CFD infra is not wired in this fixture — the switch must be refused
    // instead of leaving the engine in a half-switched state.
    const auto j = m_services->switchDirection("cfd");
    EXPECT_TRUE(j.contains("error"));
    EXPECT_EQ("futures", m_flow->activeMarket() == MarketType::Futures
                             ? "futures" : "other");
}

TEST_F(EngineServicesTest, SwitchDirectionRejectsUnknownDirection)
{
    const auto j = m_services->switchDirection("options");
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(EngineServicesTest, SwitchDirectionToSpotSwitchesState)
{
    // This fixture wires only the spot REST client, so spot is switchable.
    const auto j = m_services->switchDirection("spot");
    ASSERT_FALSE(j.contains("error")) << j.dump();
    EXPECT_EQ("futures", j["switched_from"].get<std::string>());
    EXPECT_EQ("spot", j["switched_to"].get<std::string>());
    EXPECT_EQ(MarketType::Spot, m_flow->activeMarket());
}

TEST_F(EngineServicesTest, SwitchDirectionNoopKeepsState)
{
    // A second switch to the same direction is a no-op returning status JSON.
    const auto j1 = m_services->switchDirection("spot");
    ASSERT_FALSE(j1.contains("error")) << j1.dump();
    const auto j2 = m_services->switchDirection("spot");
    ASSERT_FALSE(j2.contains("error")) << j2.dump();
    EXPECT_EQ("spot", j2["switched_to"].get<std::string>());
    EXPECT_EQ(0, j2["cancelled_orders"].get<int>());
}

TEST_F(EngineServicesTest, OpenOrderDefaultsToActiveMarket)
{
    // market_type omitted → the active direction (futures) is used, so the
    // futures FakePlacer receives the order.
    auto result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 1.0 },
    });
    // The futures placer is null in this fixture — expect the no-executor
    // error, NOT an InactiveMarket gate rejection (proves defaulting worked).
    ASSERT_FALSE(ok(result));
    EXPECT_NE(ErrorCode::InactiveMarket, error(result).code);
}

TEST_F(EngineServicesTest, MarketSelectsFeedByMarketType)
{
    // No feeds wired — explicit cfd market_type must still produce a feed
    // lookup result (error) rather than crashing.
    const auto j = m_services->market("XAUUSD", 5, 0, "cfd");
    EXPECT_TRUE(j.contains("error"));
}

TEST_F(EngineServicesTest, SignalsEmptyBoard)
{
    const auto j = m_services->signals();
    ASSERT_TRUE(j.contains("signals"));
    EXPECT_EQ(0u, j["signals"].size());
    EXPECT_TRUE(j["aggregate"].is_null());
}

TEST_F(EngineServicesTest, SignalsReflectPublishedEntries)
{
    TradingSignal sig;
    sig.type = SignalType::Sell;
    sig.symbol = "XAUUSD";
    sig.confidence = 0.8;
    sig.price = 4401.5;
    sig.strategy_id = "momentum_scalper_XAUUSD";
    sig.reason = "unit test";
    sig.market_type = MarketType::Cfd;
    sig.indicators = { { "ema_fast", 4401.0 }, { "ema_slow", 4399.0 } };
    m_board->publish(sig);

    const auto j = m_services->signals();
    ASSERT_EQ(1u, j["signals"].size());
    const auto &e = j["signals"][0];
    EXPECT_EQ("momentum_scalper_XAUUSD", e["source"].get<std::string>());
    EXPECT_EQ("sell", e["type"].get<std::string>());
    EXPECT_DOUBLE_EQ(0.8, e["confidence"].get<double>());
    EXPECT_EQ("cfd", e["market_type"].get<std::string>());
    // The display-timezone companion must exist.
    EXPECT_FALSE(e["ts_str"].get<std::string>().empty());
    EXPECT_DOUBLE_EQ(4401.0, e["indicators"]["ema_fast"].get<double>());
}

TEST_F(EngineServicesTest, GetSignalsRegisteredInMethodRegistry)
{
    m_board->publishAggregate([]()
    {
        TradingSignal sig;
        sig.type = SignalType::Buy;
        sig.symbol = "XAUUSD";
        sig.confidence = 0.72;
        sig.price = 4402.0;
        sig.strategy_id = "aggregate";
        sig.reason = "unit test";
        sig.market_type = MarketType::Cfd;
        return sig;
    }());

    const auto reg = makeMethodRegistry(*m_services);
    const auto it = reg.find("get_signals");
    ASSERT_NE(reg.end(), it);
    const auto result = it->second(nlohmann::json::object());
    const auto payload = std::get<nlohmann::json>(result);
    ASSERT_TRUE(payload["aggregate"].is_object());
    EXPECT_DOUBLE_EQ(0.72, payload["aggregate"]["confidence"].get<double>());
    // Board was constructed with StrategyConfig's default threshold (0.7).
    EXPECT_DOUBLE_EQ(0.7, payload["aggregate"]["threshold"].get<double>());
}
