// test_engine_services.cpp — Unit tests for EngineServices (empty-state)

#include "control/EngineServices.hpp"
#include "control/JsonRpcServer.hpp"
#include "control/OrderFlowExecutor.hpp"

#include "core/TimeUtil.hpp"
#include "strategy/signal/SignalBoard.hpp"
#include "execution/OrderTracker.hpp"
#include "trade_recorder/TradeRecorder.hpp"
#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/OrderRateLimiter.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <filesystem>

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

// ---------------------------------------------------------------------------
// M23: futures trigger orders (price_orders)
// ---------------------------------------------------------------------------
TEST_F(EngineServicesTest, PlaceTriggerOrderRequiresContract)
{
    auto result = m_services->placeTriggerOrder(nlohmann::json::object());
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST_F(EngineServicesTest, PlaceTriggerOrderRequiresPositivePrice)
{
    auto result = m_services->placeTriggerOrder(
        nlohmann::json{ { "contract", "SNDK_USDT" },
                        { "trigger_price", 0.0 } });
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST_F(EngineServicesTest, PlaceTriggerOrderRejectsNonZeroSize)
{
    // Gate price_orders supports whole-position closes only (2026-08-18
    // live probes: 1017/1014/1021). Partial closes go through open_order
    // with reduce_only — reject non-zero size with clear guidance. Wire the
    // REST client onto the futures slot so the size check (which sits after
    // the wiring check) is reached.
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, m_restClient.get(), nullptr,
        nullptr, nullptr, nullptr, *m_flow, *m_board, m_restMutex);

    auto result = svc.placeTriggerOrder(
        nlohmann::json{ { "contract", "SNDK_USDT" },
                        { "trigger_price", 1720.0 },
                        { "rule", 2 },
                        { "size", 2 },
                        { "order_type", "close-short-position" } });
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
    EXPECT_NE(std::string::npos,
              error(result).message.find("whole-position closes only"));
}

TEST_F(EngineServicesTest, PlaceTriggerOrderReachesRestLayer)
{
    // The fixture wires its REST client to the spot slot; build a services
    // instance with it on the FUTURES slot. The fixture has no API
    // credentials, so the request must fail deterministically at the REST
    // layer ("Missing API key") — proving the body made it past validation
    // and serialization, not with a validation error.
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, m_restClient.get(), nullptr,
        nullptr, nullptr, nullptr, *m_flow, *m_board, m_restMutex);

    auto result = svc.placeTriggerOrder(
        nlohmann::json{ { "contract", "SNDK_USDT" },
                        { "trigger_price", 1720.0 },
                        { "rule", 2 },
                        { "size", 0 },
                        { "order_type", "close-short-position" } });
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::HttpError, error(result).code);
    EXPECT_NE(std::string::npos,
              error(result).message.find("Missing API key"));
}

TEST_F(EngineServicesTest, ListTriggerOrdersRequiresContract)
{
    auto result = m_services->listTriggerOrders(nlohmann::json::object());
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST_F(EngineServicesTest, CancelTriggerOrderRequiresId)
{
    auto result = m_services->cancelTriggerOrder(nlohmann::json::object());
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
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

// ---------------------------------------------------------------------------
// M21 — hot position sync + dynamic SL/TP
// ---------------------------------------------------------------------------

TEST_F(EngineServicesTest, ModifySlTpRejectsUnknownPosition)
{
    const auto result = m_services->modifySlTp(
        nlohmann::json{ { "position_id", "no_such_position" },
                        { "sl_price", 4300.0 } });
    ASSERT_FALSE(ok(result));
    EXPECT_NE(std::string::npos, error(result).message.find("not found"));
}

TEST_F(EngineServicesTest, ModifySlTpRequiresAtLeastOneField)
{
    const auto result = m_services->modifySlTp(
        nlohmann::json{ { "position_id", "XAUUSD_Buy_1" } });
    ASSERT_FALSE(ok(result));
    EXPECT_NE(std::string::npos,
              error(result).message.find("sl_price/tp_price"));
}

TEST_F(EngineServicesTest, ModifySlTpRejectsNonCfdPosition)
{
    const auto opened = m_positionMgr->openPosition(
        "BTC_USDT", Side::Sell, 0.007, 63075.5, "s1");
    ASSERT_TRUE(ok(opened));

    const auto result = m_services->modifySlTp(
        nlohmann::json{ { "position_id", value(opened) },
                        { "sl_price", 63000.0 } });
    ASSERT_FALSE(ok(result));
    EXPECT_NE(std::string::npos,
              error(result).message.find("CFD positions"));
}

TEST_F(EngineServicesTest, ModifySlTpOnCfdPositionRequiresInfrastructure)
{
    // CFD-only validation passes; the fixture wires no CFD REST client, so
    // the method must fail with the infrastructure error (not crash).
    const auto opened = m_positionMgr->openPosition(
        "XAUUSD", Side::Buy, 0.01, 4400.0, "s1", MarketType::Cfd, 100.0,
        MarginMode::Cross, 1.0, 0.0);
    ASSERT_TRUE(ok(opened));

    const auto result = m_services->modifySlTp(
        nlohmann::json{ { "position_id", value(opened) },
                        { "sl_price", 4395.0 },
                        { "tp_price", 4408.0 } });
    ASSERT_FALSE(ok(result));
    EXPECT_NE(std::string::npos,
              error(result).message.find("infrastructure"));
}

TEST_F(EngineServicesTest, PositionsIncludeAttachedSlTp)
{
    // A CFD position synced from the exchange carries its native
    // price_sl / price_tp — get_positions must surface them so consumers
    // (sub-agent) can read the attached protective stops.
    m_positionMgr->syncPositionFromExchange(
        "XAUUSD", Side::Buy, 0.01, 4396.01, 4396.01, MarketType::Cfd, 500.0,
        MarginMode::Cross, 1.0, 0.0, 0.0, Timestamp{}, 4391.33, 4405.33);

    const auto j = m_services->positions();
    ASSERT_EQ(1, j["positions"].size());
    const auto &p = j["positions"][0];
    EXPECT_DOUBLE_EQ(4391.33, p["sl_price"].get<double>());
    EXPECT_DOUBLE_EQ(4405.33, p["tp_price"].get<double>());
}

TEST_F(EngineServicesTest, SyncPositionsHandlesUnconfiguredRest)
{
    // No futures/CFD REST clients are wired in this fixture — the sync must
    // return a zeroed summary instead of crashing or erroring.
    const auto summary = m_services->syncPositions();
    EXPECT_TRUE(summary.is_object());
    EXPECT_EQ(0, summary["futures_synced"].get<int>());
    EXPECT_EQ(0, summary["cfd_synced"].get<int>());
    EXPECT_EQ(0, summary["pruned"].get<int>());
}

TEST_F(EngineServicesTest, FuturesGhostPruneNeverRunsOnFailedListing)
{
    // M26: futures ghost pruning (fill-tracked positions on contracts the
    // exchange no longer holds) must ONLY run against a SUCCESSFUL exchange
    // listing. The fixture has no API credentials, so the futures sync fails
    // fast at the REST layer — seed a stale-looking fill-tracked ETH grid
    // position and verify it survives the sync untouched. A prune that ran
    // on failure would drop real positions.
    const auto opened = m_positionMgr->openPosition(
        "ETH_USDT", Side::Sell, 2.0, 2050.0, "eth-grid-2050",
        MarketType::Futures, 10.0, MarginMode::Cross, 0.01, 0.0);
    ASSERT_TRUE(ok(opened));

    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, m_restClient.get(), nullptr,
        nullptr, nullptr, nullptr, *m_flow, *m_board, m_restMutex);

    const auto summary = svc.syncPositions();
    EXPECT_EQ(0, summary["futures_synced"].get<int>());
    const auto j = svc.positions();
    ASSERT_EQ(1, j["positions"].size());
    EXPECT_EQ("eth-grid-2050",
              j["positions"][0]["strategy_id"].get<std::string>());
}

TEST_F(EngineServicesTest, OpenOrderCfdWithSlRejectedWhenFreeMarginFloorUnmet)
{
    // M22 minimum-free-margin-after-stop gate: with a floor configured, a CFD
    // order carrying an attached SL must leave available - margin - stop-loss
    // >= floor. The fixture's REST client has no credentials, so the balance
    // fetch fails and available is treated as 0 — any SL order is rejected.
    // A local flow with the fake CFD placer keeps the test off the network.
    m_cfg.risk.minAvailableAfterStopUsd = 6.0;
    OrderFlowExecutor flow(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_placer.get(), m_placer.get(), m_placer.get(),
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restClient.get(),  // spot / futures / CFD rest
        nullptr, nullptr, nullptr, flow, *m_board, m_restMutex);

    const auto result = svc.openOrder(nlohmann::json{
        { "symbol", "XAUUSD" },
        { "side", "buy" },
        { "quantity", 0.01 },
        { "market_type", "cfd" },
        { "type", "limit" },
        { "price", 4395.0 },
        { "sl_price", 4390.0 },
    });
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::InsufficientFreeMargin, error(result).code);
    EXPECT_NE(std::string::npos,
              error(result).message.find("minAvailableAfterStopUsd"));
}

TEST_F(EngineServicesTest, OpenOrderCfdWithSlUsesConfiguredLeverageForMargin)
{
    // Regression (2026-08-18 round 152): manual CFD orders don't carry a
    // leverage field; the gate assumed 1:1 and computed the full notional as
    // margin (43.95 USD here with the fixture's 1.0 quanto), blocking every
    // order. The margin must fall back to the configured instance leverage
    // (500x → 0.0879 USD in this fixture, 8.79 USD live at quanto 100).
    StrategyInstanceConfig inst;
    inst.name = "momentum_scalper";
    inst.symbol = "XAUUSD";
    inst.market_type = MarketType::Cfd;
    inst.leverage = 500.0;
    inst.enabled = true;
    m_cfg.strategy.strategies.push_back(inst);
    m_cfg.risk.minAvailableAfterStopUsd = 6.0;
    OrderFlowExecutor flow(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_placer.get(), m_placer.get(), m_placer.get(),
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restClient.get(),
        nullptr, nullptr, nullptr, flow, *m_board, m_restMutex);

    const auto result = svc.openOrder(nlohmann::json{
        { "symbol", "XAUUSD" },
        { "side", "buy" },
        { "quantity", 0.01 },
        { "market_type", "cfd" },
        { "type", "limit" },
        { "price", 4395.0 },
        { "sl_price", 4390.0 },
    });
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::InsufficientFreeMargin, error(result).code);
    // 4395 * 0.01 * 1.0 (fixture quanto) / 500 = 0.0879 — not 43.95 full notional.
    EXPECT_NE(std::string::npos, error(result).message.find("margin 0.087900"));
}

TEST_F(EngineServicesTest, OpenOrderCfdWithSlNoInstanceFallsBackToFullNotional)
{
    // No matching CFD instance configured: the gate conservatively assumes
    // 1:1 margin (full notional) rather than guessing a leverage.
    m_cfg.risk.minAvailableAfterStopUsd = 6.0;
    OrderFlowExecutor flow(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_placer.get(), m_placer.get(), m_placer.get(),
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restClient.get(),
        nullptr, nullptr, nullptr, flow, *m_board, m_restMutex);

    const auto result = svc.openOrder(nlohmann::json{
        { "symbol", "XAUUSD" },
        { "side", "buy" },
        { "quantity", 0.01 },
        { "market_type", "cfd" },
        { "type", "limit" },
        { "price", 4395.0 },
        { "sl_price", 4390.0 },
    });
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::InsufficientFreeMargin, error(result).code);
    EXPECT_NE(std::string::npos, error(result).message.find("margin 43.950000"));
}

TEST_F(EngineServicesTest, OpenOrderCfdWithoutSlBypassesGate)
{
    // Same floor configured, but no attached stop-loss — the gate does not
    // apply (nothing guarantees a bounded loss), the order flows through.
    m_cfg.risk.minAvailableAfterStopUsd = 6.0;
    OrderFlowExecutor flow(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_placer.get(), m_placer.get(), m_placer.get(),
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restClient.get(),
        nullptr, nullptr, nullptr, flow, *m_board, m_restMutex);

    const auto result = svc.openOrder(nlohmann::json{
        { "symbol", "XAUUSD" },
        { "side", "buy" },
        { "quantity", 0.01 },
        { "market_type", "cfd" },
        { "type", "limit" },
        { "price", 4395.0 },
    });
    ASSERT_TRUE(ok(result));
}

TEST_F(EngineServicesTest, OpenOrderCfdWithSlGateDisabledByDefault)
{
    // Floor unset (0) — the gate is off entirely; SL orders pass.
    OrderFlowExecutor flow(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_placer.get(), m_placer.get(), m_placer.get(),
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restClient.get(),
        nullptr, nullptr, nullptr, flow, *m_board, m_restMutex);

    const auto result = svc.openOrder(nlohmann::json{
        { "symbol", "XAUUSD" },
        { "side", "sell" },
        { "quantity", 0.01 },
        { "market_type", "cfd" },
        { "type", "limit" },
        { "price", 4400.0 },
        { "sl_price", 4405.0 },
    });
    ASSERT_TRUE(ok(result));
}

#ifdef PULSE_ENABLE_SQLITE
TEST_F(EngineServicesTest, ExternalCloseTracesIntoTradesDb)
{
    // M22 external-close trace: recordExternalClose must land the close side
    // in trades.db with the ext_ marker, even though the engine never saw an
    // order for it (user manual close / exchange-side SL fill).
    const std::string db_path = "/tmp/pulse_ext_close_test.db";
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");

    auto recorder = trade_recorder::TradeRecorder::open(db_path);
    ASSERT_TRUE(ok(recorder));

    OrderFlowExecutor flow(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        m_placer.get(), nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, m_restMutex, &value(recorder));

    execution::ExecutionReport report;
    report.order_id = "ext_close_XAUUSD_Buy_1";
    report.client_order_id = report.order_id;
    report.symbol = "XAUUSD";
    report.side = Side::Sell;
    report.type = OrderType::Market;
    report.requested_qty = 0.01;
    report.filled_qty = 0.01;
    report.avg_fill_price = 4390.1;
    report.submit_mid_price = 4390.1;
    report.slippage_bps = 0.0;
    report.fees = 0.0;
    report.latency = std::chrono::milliseconds{ 0 };
    report.submit_time = Timestamp::clock::now();
    report.fill_time = report.submit_time;
    report.final_status = OrderStatus::Filled;

    flow.recordExternalClose(report, -5.53, MarketType::Cfd, 500.0);

    const auto trades = value(recorder).getTrades("XAUUSD");
    ASSERT_TRUE(ok(trades));
    const auto &rows = value(trades);
    ASSERT_EQ(1, rows.size());
    EXPECT_EQ("ext_close_XAUUSD_Buy_1", rows[0].order_id);
    EXPECT_DOUBLE_EQ(-5.53, rows[0].pnl);
    EXPECT_EQ("XAUUSD", rows[0].symbol);
}
#endif // PULSE_ENABLE_SQLITE

TEST_F(EngineServicesTest, SyncPositionsAndModifySlTpRegistered)
{
    const auto reg = makeMethodRegistry(*m_services);
    EXPECT_NE(reg.end(), reg.find("sync_positions"));
    EXPECT_NE(reg.end(), reg.find("modify_sl_tp"));

    // sync_positions is a no-arg query that must resolve without params.
    const auto sync_result = reg.at("sync_positions")(nlohmann::json::object());
    const auto payload = std::get<nlohmann::json>(sync_result);
    EXPECT_TRUE(payload.is_object());
}

TEST_F(EngineServicesTest, ListFuturesOrdersRequiresContract)
{
    auto result = m_services->listFuturesOrders(nlohmann::json::object());
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
}

TEST_F(EngineServicesTest, GridResumeWhenNotConfiguredFails)
{
    // The fixture wires no GridManager — the service must fail cleanly with
    // 9200 before touching the manager.
    const auto result = m_services->gridResume();
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::GridNotStarted, error(result).code);
}

// ---------------------------------------------------------------------------
// AI tuning observability — bounds clamping + audit log
// ---------------------------------------------------------------------------

namespace
{

class MockScalper : public strategy::StrategyBase
{
  public:
    explicit MockScalper(const strategy::StrategyContext &ctx)
    {
        m_context = ctx;
    }

    [[nodiscard]] std::string name() const override
    {
        return "MockScalper";
    }

    [[nodiscard]] std::string id() const override
    {
        return "mock_scalper_" + m_context.config.symbol;
    }

    [[nodiscard]] strategy::StrategyParams &params() override
    {
        return m_params;
    }

    void onTick(const market::Ticker &) override
    {
    }

    void onKline(const market::Kline &) override
    {
    }

    void onOrderbook(const market::OrderBook &) override
    {
    }

    strategy::StrategyParams m_params;
};

} // anonymous namespace

TEST_F(EngineServicesTest, SetParamClampsToHardBounds)
{
    strategy::StrategyContext ctx;
    ctx.config.symbol = "BTC_USDT";
    ctx.config.market_type = MarketType::Futures;
    m_strategyMgr.registerStrategy(std::make_unique<MockScalper>(ctx));

    core::ParamChangeLog change_log;
    EngineServices svc("test", m_start, m_cfg, m_strategyMgr, *m_riskMgr,
                       *m_positionMgr, nullptr, nullptr, nullptr,
                       m_restClient.get(), nullptr, nullptr,
                       nullptr, nullptr, nullptr, *m_flow, *m_board,
                       m_restMutex, nullptr, nullptr, &change_log);

    // min_confidence hard_max = 0.95; hard_min = 0.1.
    EXPECT_TRUE(svc.setStrategyParam("mock_scalper_BTC_USDT", "min_confidence", 0.99));
    const auto params = svc.getStrategyParams("mock_scalper_BTC_USDT");
    EXPECT_DOUBLE_EQ(0.95, params["min_confidence"].get<double>());

    EXPECT_TRUE(svc.setStrategyParam("mock_scalper_BTC_USDT", "min_confidence", 0.05));
    EXPECT_DOUBLE_EQ(0.1, svc.getStrategyParams("mock_scalper_BTC_USDT")["min_confidence"].get<double>());
}

TEST_F(EngineServicesTest, SetParamWithinBoundsPassThrough)
{
    strategy::StrategyContext ctx;
    ctx.config.symbol = "BTC_USDT";
    ctx.config.market_type = MarketType::Futures;
    m_strategyMgr.registerStrategy(std::make_unique<MockScalper>(ctx));

    core::ParamChangeLog change_log;
    EngineServices svc("test", m_start, m_cfg, m_strategyMgr, *m_riskMgr,
                       *m_positionMgr, nullptr, nullptr, nullptr,
                       m_restClient.get(), nullptr, nullptr,
                       nullptr, nullptr, nullptr, *m_flow, *m_board,
                       m_restMutex, nullptr, nullptr, &change_log);

    EXPECT_TRUE(svc.setStrategyParam("mock_scalper_BTC_USDT", "min_confidence", 0.5));
    EXPECT_DOUBLE_EQ(0.5, svc.getStrategyParams("mock_scalper_BTC_USDT")["min_confidence"].get<double>());
}

TEST_F(EngineServicesTest, SetParamUnboundedPassThrough)
{
    strategy::StrategyContext ctx;
    ctx.config.symbol = "BTC_USDT";
    ctx.config.market_type = MarketType::Futures;
    m_strategyMgr.registerStrategy(std::make_unique<MockScalper>(ctx));

    core::ParamChangeLog change_log;
    EngineServices svc("test", m_start, m_cfg, m_strategyMgr, *m_riskMgr,
                       *m_positionMgr, nullptr, nullptr, nullptr,
                       m_restClient.get(), nullptr, nullptr,
                       nullptr, nullptr, nullptr, *m_flow, *m_board,
                       m_restMutex, nullptr, nullptr, &change_log);

    // supertrend_period has no entry in the shared bounds table → passthrough.
    EXPECT_TRUE(svc.setStrategyParam("mock_scalper_BTC_USDT", "supertrend_period", 500.0));
    EXPECT_DOUBLE_EQ(500.0, svc.getStrategyParams("mock_scalper_BTC_USDT")["supertrend_period"].get<double>());
}

TEST_F(EngineServicesTest, SetParamRecordsManualAudit)
{
    strategy::StrategyContext ctx;
    ctx.config.symbol = "BTC_USDT";
    ctx.config.market_type = MarketType::Futures;
    m_strategyMgr.registerStrategy(std::make_unique<MockScalper>(ctx));

    core::ParamChangeLog change_log;
    EngineServices svc("test", m_start, m_cfg, m_strategyMgr, *m_riskMgr,
                       *m_positionMgr, nullptr, nullptr, nullptr,
                       m_restClient.get(), nullptr, nullptr,
                       nullptr, nullptr, nullptr, *m_flow, *m_board,
                       m_restMutex, nullptr, nullptr, &change_log);

    EXPECT_TRUE(svc.setStrategyParam("mock_scalper_BTC_USDT", "min_confidence", 0.7));
    const auto snap = change_log.snapshot();
    ASSERT_EQ(1u, snap.size());
    EXPECT_EQ("manual", snap[0].source);
    EXPECT_EQ("mock_scalper_BTC_USDT", snap[0].strategy_id);
    EXPECT_EQ("min_confidence", snap[0].param_name);
    EXPECT_DOUBLE_EQ(0.6, snap[0].old_value); // default min_confidence
    EXPECT_DOUBLE_EQ(0.7, snap[0].new_value);
}

TEST_F(EngineServicesTest, ParamHistoryReturnsRecentFirst)
{
    strategy::StrategyContext ctx;
    ctx.config.symbol = "BTC_USDT";
    ctx.config.market_type = MarketType::Futures;
    m_strategyMgr.registerStrategy(std::make_unique<MockScalper>(ctx));

    core::ParamChangeLog change_log;
    EngineServices svc("test", m_start, m_cfg, m_strategyMgr, *m_riskMgr,
                       *m_positionMgr, nullptr, nullptr, nullptr,
                       m_restClient.get(), nullptr, nullptr,
                       nullptr, nullptr, nullptr, *m_flow, *m_board,
                       m_restMutex, nullptr, nullptr, &change_log);

    (void)svc.setStrategyParam("mock_scalper_BTC_USDT", "min_confidence", 0.61);
    (void)svc.setStrategyParam("mock_scalper_BTC_USDT", "min_confidence", 0.62);

    const auto history = svc.paramHistory();
    ASSERT_TRUE(history.is_array());
    ASSERT_EQ(2u, history.size());
    EXPECT_DOUBLE_EQ(0.62, history[0]["new_value"].get<double>()); // newest first
    EXPECT_DOUBLE_EQ(0.61, history[1]["new_value"].get<double>());
    EXPECT_EQ("manual", history[0]["source"].get<std::string>());
}
