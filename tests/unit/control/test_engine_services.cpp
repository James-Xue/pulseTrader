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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

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
        ++place_calls;
        if (scripted_result.has_value())
        {
            // One-shot, like a real exchange response.
            auto result = *scripted_result;
            scripted_result.reset();
            return result;
        }
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

    /// Number of orders that actually reached the placer. The idempotency
    /// tests assert this stays at 1 for repeated client_order_ids.
    int place_calls{ 0 };

    /// Optional scripted outcome for the next place() call (success response
    /// or an exchange/transport PulseError).
    std::optional<Result<execution::OrderResponse>> scripted_result;
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

        // REST-poll-only trackers (no private WS) so the orders() merge can be
        // exercised without a socket. They stay empty unless a test tracks an
        // order, and reconcileAll() over an empty map performs no REST call.
        m_spotTracker = std::make_unique<execution::OrderTracker>(
            nullptr, *m_restClient, MarketType::Spot, false);
        m_futuresTracker = std::make_unique<execution::OrderTracker>(
            nullptr, *m_restClient, MarketType::Futures, false);
        m_cfdTracker = std::make_unique<execution::OrderTracker>(
            nullptr, *m_restClient, MarketType::Cfd, false);

        m_services = std::make_unique<EngineServices>(
            "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
            nullptr, nullptr, nullptr,
            m_restClient.get(), nullptr, nullptr,
            m_spotTracker.get(), m_futuresTracker.get(), m_cfdTracker.get(),
            *m_flow, *m_board, m_restMutex);
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
    std::unique_ptr<execution::OrderTracker> m_spotTracker;
    std::unique_ptr<execution::OrderTracker> m_futuresTracker;
    std::unique_ptr<execution::OrderTracker> m_cfdTracker;
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

TEST_F(EngineServicesTest, OrdersStampMarketType)
{
    // One order per tracker. The merge must preserve the market identity that
    // each tracker knows and the merged snapshot would otherwise lose — the
    // Qt client uses it to put an order on the spot / CFD / futures tab.
    m_spotTracker->trackOrder("spot-1", "BTC_USDT", Side::Buy, OrderType::Limit,
                              1.0, 100.0);
    m_futuresTracker->trackOrder("fut-1", "BTC_USDT", Side::Sell,
                                 OrderType::Limit, 2.0, 100.0);
    m_cfdTracker->trackOrder("cfd-1", "XAUUSD", Side::Buy, OrderType::Market,
                             3.0, 2000.0);

    // reconcile=false keeps the test off the network; tagging happens on the
    // merge regardless.
    const auto j = m_services->orders(false);
    const auto &active = j["activeOrders"];
    ASSERT_EQ(3, active.size());

    // Blocks are appended in spot → futures → cfd order.
    EXPECT_EQ("spot", active[0]["market_type"]);
    EXPECT_EQ("spot-1", active[0]["order_id"]);
    EXPECT_EQ("futures", active[1]["market_type"]);
    EXPECT_EQ("fut-1", active[1]["order_id"]);
    EXPECT_EQ("cfd", active[2]["market_type"]);
    EXPECT_EQ("cfd-1", active[2]["order_id"]);

    // Recent fills go through the same merge and need the same tag.
    execution::ExecutionReport fill;
    fill.order_id = "spot-fill";
    fill.symbol = "BTC_USDT";
    fill.side = Side::Buy;
    fill.type = OrderType::Market;
    fill.requested_qty = 1.0;
    fill.filled_qty = 1.0;
    fill.avg_fill_price = 100.0;
    fill.final_status = OrderStatus::Filled;
    m_spotTracker->recordCompletedReport(fill);

    const auto tagged = m_services->orders(false);
    const auto &fills = tagged["recentReports"];
    ASSERT_EQ(1, fills.size());
    EXPECT_EQ("spot", fills[0]["market_type"]);
    EXPECT_EQ("spot-fill", fills[0]["order_id"]);
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
// open_order idempotency (client_order_id)
// ---------------------------------------------------------------------------
TEST_F(EngineServicesTest, OpenOrderCallerSizeRequiresClientOrderId)
{
    // A caller-authoritative size with no key cannot be de-duplicated: a
    // retry after a timeout would place a second order of exactly that size.
    // The independent external-order gate sits BEFORE this check, so arm it
    // here to keep this test focused on the idempotency contract.
    m_services->setExternalOrdersEnabled(true);
    m_flow->setActiveMarket(MarketType::Spot);

    const auto result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "size_authority", "caller" },
    });

    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
    EXPECT_EQ("open_order: client_order_id is required when size_authority is "
              "\"caller\".",
              error(result).message);
    EXPECT_EQ(0, m_placer->place_calls);
}

TEST_F(EngineServicesTest, FuturesReduceOnlyCallerSizeNeedsNoKey)
{
    // A futures reduce_only close is exempt from the key requirement because
    // Gate enforces the flag: the order cannot exceed the open position, so
    // it is bounded even without de-duplication. The shared fixture wires
    // only the spot placer, so build a futures-capable stack here.
    OrderFlowExecutor flow(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, m_placer.get(), nullptr,
        nullptr, m_futuresTracker.get(), nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr, m_futuresTracker.get(), nullptr, flow, *m_board, m_restMutex);

    const auto result = svc.openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "sell" },
        { "quantity", 1.0 },
        { "market_type", "futures" },
        { "size_authority", "caller" },
        { "reduce_only", true },
    });

    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_placer->place_calls);
}

// ---------------------------------------------------------------------------
// Independent external-order gate ([control] external_orders_enabled)
// ---------------------------------------------------------------------------
TEST_F(EngineServicesTest, CallerSizeRejectedWhileExternalOrdersDisabled)
{
    // Default OFF (fail-safe): the caller-sized path — what an external
    // runtime uses — must be refused before anything reaches the placer.
    // Assert the SPECIFIC gate error: a later stage could also fail for
    // unrelated reasons (risk halt, missing executor), which would make a
    // bare "some error" assertion pass for the wrong reason.
    m_flow->setActiveMarket(MarketType::Spot);
    ASSERT_FALSE(m_services->externalOrdersEnabled());
    EXPECT_FALSE(m_services->status()["external_orders_enabled"].get<bool>());

    const auto result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "size_authority", "caller" },
        { "client_order_id", "gt-ext-off-1" },
    });

    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
    EXPECT_NE(std::string::npos,
              error(result).message.find("[control] external_orders_enabled"));
    EXPECT_EQ(0, m_placer->place_calls);
}

TEST_F(EngineServicesTest, CallerSizeAllowedWhenExternalOrdersEnabled)
{
    m_services->setExternalOrdersEnabled(true);
    m_flow->setActiveMarket(MarketType::Spot);
    EXPECT_TRUE(m_services->externalOrdersEnabled());
    EXPECT_TRUE(m_services->status()["external_orders_enabled"].get<bool>());

    const auto result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "size_authority", "caller" },
        { "client_order_id", "gt-ext-on-1" },
    });

    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, CallerFuturesReduceOnlyBypassesExternalGate)
{
    // Disabled gate, caller authority, FUTURES reduce_only=true: a close must
    // stay possible, or flipping the flag could trap an open position. The
    // exemption is futures-only because the exchange is what enforces it.
    OrderFlowExecutor flow(
        m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
        nullptr, m_placer.get(), nullptr,
        nullptr, m_futuresTracker.get(), nullptr,
        nullptr, nullptr, m_restMutex, nullptr);
    EngineServices svc(
        "test", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
        nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr,
        nullptr, m_futuresTracker.get(), nullptr, flow, *m_board, m_restMutex);
    ASSERT_FALSE(svc.externalOrdersEnabled());

    const auto result = svc.openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "sell" },
        { "quantity", 1.0 },
        { "market_type", "futures" },
        { "size_authority", "caller" },
        { "reduce_only", true },
        { "client_order_id", "gt-ext-close-1" },
    });

    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, SpotReduceOnlyIsRejectedOutright)
{
    // The spot order body never carries reduce_only to Gate, so the flag is a
    // caller claim that cannot bound the order. Accepting it would open new
    // exposure while also exempting the order from the risk layer's drawdown
    // halt and position-count checks. It is refused before the external gate
    // and the idempotency cache are even consulted.
    m_flow->setActiveMarket(MarketType::Spot);
    ASSERT_FALSE(m_services->externalOrdersEnabled());

    const auto result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "size_authority", "caller" },
        { "reduce_only", true },
        { "client_order_id", "gt-spot-close-1" },
    });

    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(result).code);
    EXPECT_NE(std::string::npos,
              error(result).message.find("reduce_only is futures-only"));
    EXPECT_EQ(0, m_placer->place_calls);

    // Engine-authority one-shots are refused the same way: the flag's meaning
    // does not depend on who sized the order.
    const auto engine_sized = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "sell" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "reduce_only", true },
    });
    ASSERT_FALSE(ok(engine_sized));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(engine_sized).code);
    EXPECT_EQ(0, m_placer->place_calls);
}

TEST_F(EngineServicesTest, EngineAuthorityAllowedWhileExternalOrdersDisabled)
{
    // The manual path is engine-sized. The gate must not touch it even with
    // the external flag off — otherwise disabling the external runtime would
    // also disable the REPL / MCP / UI.
    m_flow->setActiveMarket(MarketType::Spot);
    ASSERT_FALSE(m_services->externalOrdersEnabled());

    const auto result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
    });

    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, ExternalGateTogglesAtRuntime)
{
    // The operational point of the flag: arm the external runtime, then kill
    // it without an engine restart. Distinct client_order_ids keep idempotency
    // replay out of the assertions; the first (rejected) request proves the
    // gate does not consume the key, because the same request is placed once
    // the gate opens.
    m_flow->setActiveMarket(MarketType::Spot);

    auto params = nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "size_authority", "caller" },
        { "client_order_id", "gt-ext-toggle-1" },
    };

    const auto disabled = m_services->openOrder(params);
    ASSERT_FALSE(ok(disabled));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(disabled).code);
    EXPECT_NE(std::string::npos,
              error(disabled).message.find("[control] external_orders_enabled"));

    m_services->setExternalOrdersEnabled(true);
    const auto armed = m_services->openOrder(params);
    ASSERT_TRUE(ok(armed)) << error(armed).message;
    EXPECT_EQ(1, m_placer->place_calls);

    m_services->setExternalOrdersEnabled(false);
    params["client_order_id"] = "gt-ext-toggle-2";
    const auto killed = m_services->openOrder(params);
    ASSERT_FALSE(ok(killed));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(killed).code);
    EXPECT_NE(std::string::npos,
              error(killed).message.find("[control] external_orders_enabled"));
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, SetExternalOrdersRegisteredInMethodRegistry)
{
    // Method names on the control socket ARE the MCP tool names, so the
    // registry entry must exist and be a thin boolean setter.
    const auto reg = makeMethodRegistry(*m_services);
    const auto it = reg.find("set_external_orders");
    ASSERT_NE(reg.end(), it);

    const auto enabled = it->second(nlohmann::json{ { "enabled", true } });
    ASSERT_TRUE(std::holds_alternative<nlohmann::json>(enabled));
    EXPECT_TRUE(
        std::get<nlohmann::json>(enabled)["external_orders_enabled"].get<bool>());
    EXPECT_TRUE(m_services->externalOrdersEnabled());

    const auto disabled = it->second(nlohmann::json{ { "enabled", false } });
    ASSERT_TRUE(std::holds_alternative<nlohmann::json>(disabled));
    EXPECT_FALSE(
        std::get<nlohmann::json>(disabled)["external_orders_enabled"].get<bool>());
    EXPECT_FALSE(m_services->externalOrdersEnabled());

    // Non-boolean / missing enabled → invalid request, state untouched.
    const auto wrong_type = it->second(nlohmann::json{ { "enabled", "yes" } });
    EXPECT_TRUE(std::holds_alternative<PulseError>(wrong_type));
    const auto missing = it->second(nlohmann::json::object());
    EXPECT_TRUE(std::holds_alternative<PulseError>(missing));
    EXPECT_FALSE(m_services->externalOrdersEnabled());
}

TEST_F(EngineServicesTest, OpenOrderReplaysSameClientOrderId)
{
    // The core guarantee: the retry returns the first attempt's OrderResponse
    // and never reaches the order flow a second time.
    m_flow->setActiveMarket(MarketType::Spot);

    const auto params = nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "client_order_id", "gt-retry-1" },
    };

    const auto first = m_services->openOrder(params);
    ASSERT_TRUE(ok(first)) << error(first).message;

    const auto second = m_services->openOrder(params);
    ASSERT_TRUE(ok(second)) << error(second).message;

    EXPECT_EQ(value(first).order_id, value(second).order_id);
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, OpenOrderRejectsClientOrderIdReuseWithDifferentParams)
{
    // Same key, different quantity — the caller is describing a different
    // order, so it must be refused rather than replayed.
    m_flow->setActiveMarket(MarketType::Spot);

    nlohmann::json params{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "client_order_id", "gt-reuse-1" },
    };
    const auto first = m_services->openOrder(params);
    ASSERT_TRUE(ok(first)) << error(first).message;

    params["quantity"] = 0.002;
    const auto second = m_services->openOrder(params);
    ASSERT_FALSE(ok(second));
    EXPECT_EQ(ErrorCode::ControlInvalidRequest, error(second).code);
    EXPECT_EQ("open_order: client_order_id reused with different order "
              "parameters.",
              error(second).message);
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, OpenOrderEmitsSubmittedEventOncePerPlacedOrder)
{
    // The push channel announces a placed order exactly once. A replayed
    // idempotent retry placed nothing new, so it must not announce a second
    // order; a rejected request never reached the exchange, so it stays
    // silent as well.
    std::vector<nlohmann::json> events;
    m_services->setOrderEventSink(
        [&events](const nlohmann::json &event) { events.push_back(event); });
    m_flow->setActiveMarket(MarketType::Spot);

    const auto params = nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "price", 65000.0 },
        { "type", "limit" },
        { "market_type", "spot" },
        { "client_order_id", "evt-submit-1" },
    };

    const auto placed = m_services->openOrder(params);
    ASSERT_TRUE(ok(placed)) << error(placed).message;

    ASSERT_EQ(1u, events.size());
    const auto &event = events[0];
    EXPECT_EQ("submitted", event["event"].get<std::string>());
    EXPECT_EQ(value(placed).order_id, event["order_id"].get<std::string>());
    EXPECT_EQ("evt-submit-1", event["client_order_id"].get<std::string>());
    EXPECT_EQ("BTC_USDT", event["symbol"].get<std::string>());
    EXPECT_EQ("spot", event["market_type"].get<std::string>());
    EXPECT_EQ("buy", event["side"].get<std::string>());
    EXPECT_EQ("limit", event["type"].get<std::string>());
    EXPECT_DOUBLE_EQ(65000.0, event["price"].get<double>());
    EXPECT_DOUBLE_EQ(0.001, event["quantity"].get<double>());
    EXPECT_DOUBLE_EQ(0.0, event["filled_quantity"].get<double>());
    EXPECT_EQ("open", event["status"].get<std::string>());
    EXPECT_GT(event["ts"].get<std::int64_t>(), 0);

    // Replay of the same key: same order, no new event.
    const auto replayed = m_services->openOrder(params);
    ASSERT_TRUE(ok(replayed)) << error(replayed).message;
    EXPECT_EQ(1u, events.size());

    // Rejected request: nothing was placed, nothing is announced.
    const auto rejected = m_services->openOrder(nlohmann::json{
        { "symbol", "" },
        { "side", "buy" },
        { "quantity", 0.001 },
    });
    EXPECT_FALSE(ok(rejected));
    EXPECT_EQ(1u, events.size());
}

TEST_F(EngineServicesTest, SubmittedEventNeverReportsATerminalStatus)
{
    // A market order's POST reply can already be Filled. The submit event
    // must still describe a non-terminal state with no fill, or a client that
    // treats `filled` as terminal records a zero-size fill and re-arms the
    // entry before the `completed` report arrives.
    std::vector<nlohmann::json> events;
    m_services->setOrderEventSink(
        [&events](const nlohmann::json &event) { events.push_back(event); });
    m_flow->setActiveMarket(MarketType::Spot);

    execution::OrderResponse filled;
    filled.order_id = "fake-filled";
    filled.status = OrderStatus::Filled;
    m_placer->scripted_result = filled;

    const auto result = m_services->openOrder(nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "client_order_id", "evt-filled-1" },
    });
    ASSERT_TRUE(ok(result)) << error(result).message;
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("submitted", events[0]["event"].get<std::string>());
    EXPECT_EQ("open", events[0]["status"].get<std::string>());
    EXPECT_DOUBLE_EQ(0.0, events[0]["filled_quantity"].get<double>());
}

TEST_F(EngineServicesTest, OpenOrderPreFlightRiskRejectionKeepsKeyRetryable)
{
    // A halt is a pre-flight veto: nothing reached the exchange and the
    // condition is transient. Caching it would pin the key to a rejection that
    // is no longer true; instead the SAME client_order_id must place the order
    // once trading resumes.
    m_flow->setActiveMarket(MarketType::Spot);

    const auto params = nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "client_order_id", "gt-halt-1" },
    };

    m_services->haltTrading();
    const auto rejected = m_services->openOrder(params);
    ASSERT_FALSE(ok(rejected)) << "a halted engine must reject the order";
    EXPECT_EQ(0, m_placer->place_calls);

    m_services->resumeTrading();
    const auto retried = m_services->openOrder(params);
    ASSERT_TRUE(ok(retried)) << "the key must stay retryable: "
                             << error(retried).message;
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, OpenOrderTransportFailureReplaysAsOutcomeUnknown)
{
    // A POST that timed out may already be live at the exchange. Replaying
    // the raw timeout would tell the caller "not placed" and invite a second
    // order; the replay must instead mark the outcome unknown and ask for a
    // reconcile. The original call still reports the real failure.
    m_flow->setActiveMarket(MarketType::Spot);
    m_placer->scripted_result =
        PulseError{ ErrorCode::NetworkTimeout, "curl timeout" };

    const auto params = nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "client_order_id", "gt-unknown-1" },
    };

    const auto first = m_services->openOrder(params);
    ASSERT_FALSE(ok(first));
    EXPECT_EQ(ErrorCode::NetworkTimeout, error(first).code);
    EXPECT_EQ(1, m_placer->place_calls);

    const auto replay = m_services->openOrder(params);
    ASSERT_FALSE(ok(replay));
    EXPECT_EQ(ErrorCode::ControlOrderOutcomeUnknown, error(replay).code);
    EXPECT_NE(std::string::npos,
              error(replay).message.find("reconcile"));
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, OpenOrderDefinitiveRejectionIsReplayedVerbatim)
{
    // An exchange rejection is terminal: nothing was created, so replaying it
    // is correct and the order must not be sent again.
    m_flow->setActiveMarket(MarketType::Spot);
    m_placer->scripted_result =
        PulseError{ ErrorCode::InvalidOrder, "order size below minimum" };

    const auto params = nlohmann::json{
        { "symbol", "BTC_USDT" },
        { "side", "buy" },
        { "quantity", 0.001 },
        { "market_type", "spot" },
        { "client_order_id", "gt-reject-1" },
    };

    const auto first = m_services->openOrder(params);
    ASSERT_FALSE(ok(first));
    const auto replay = m_services->openOrder(params);
    ASSERT_FALSE(ok(replay));
    EXPECT_EQ(error(first).code, error(replay).code);
    EXPECT_EQ(error(first).message, error(replay).message);
    EXPECT_EQ(1, m_placer->place_calls);
}

TEST_F(EngineServicesTest, OpenOrderPreFlightFailureKeepsKeyRetryable)
{
    // A request that fails BEFORE the order flow (the CFD free-margin gate)
    // must not poison the key: the guard aborts it, so the same
    // client_order_id can be evaluated again on a later retry. A leaked
    // in-flight entry would surface as the duplicate-in-flight rejection.
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

    const auto params = nlohmann::json{
        { "symbol", "XAUUSD" },
        { "side", "buy" },
        { "quantity", 0.01 },
        { "market_type", "cfd" },
        { "type", "limit" },
        { "price", 4395.0 },
        { "sl_price", 4390.0 },
        { "client_order_id", "gt-gate-1" },
    };

    const auto gated = svc.openOrder(params);
    ASSERT_FALSE(ok(gated));
    EXPECT_EQ(ErrorCode::InsufficientFreeMargin, error(gated).code);

    const auto retried = svc.openOrder(params);
    ASSERT_FALSE(ok(retried));
    EXPECT_EQ(ErrorCode::InsufficientFreeMargin, error(retried).code);
    EXPECT_EQ(0, m_placer->place_calls);
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
    // Provenance travels with every position (additive field): a synced
    // exchange position is "manual", and clients use it to distinguish
    // caller-owned ("external") positions from engine-managed ones.
    ASSERT_TRUE(p.contains("origin"));
    EXPECT_EQ("manual", p["origin"].get<std::string>());
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
    EXPECT_NE(reg.end(), reg.find("get_gold_trader"));

    // sync_positions is a no-arg query that must resolve without params.
    const auto sync_result = reg.at("sync_positions")(nlohmann::json::object());
    const auto payload = std::get<nlohmann::json>(sync_result);
    EXPECT_TRUE(payload.is_object());
}

TEST_F(EngineServicesTest, GoldTraderStatusDisabledReportsConfigAndReason)
{
    GoldTraderConfig cfg;
    cfg.enabled = true;
    cfg.symbol = "XAUUSD";
    cfg.hard_stop_usd = 8.0;
    cfg.contract_volume = 100.0;

    m_services->setGoldTrader(nullptr, cfg, "XAUUSD contract metadata unavailable");
    const auto status = m_services->goldTraderStatus();

    EXPECT_FALSE(status.value("armed", true));
    EXPECT_TRUE(status.value("enabled", false));
    EXPECT_EQ("XAUUSD", status.value("symbol", ""));
    EXPECT_EQ("cfd", status.value("market_type", ""));
    EXPECT_EQ("XAUUSD contract metadata unavailable", status.value("reason", ""));
    EXPECT_DOUBLE_EQ(8.0, status["config"].value("hard_stop_usd", 0.0));
    EXPECT_DOUBLE_EQ(100.0, status["config"].value("contract_volume", 0.0));
    EXPECT_TRUE(status["tracked_positions"].is_array());
    EXPECT_TRUE(status["tracked_positions"].empty());
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

TEST_F(EngineServicesTest, SetStrategyTradingTogglesSnapshot)
{
    strategy::StrategyContext ctx;
    ctx.config.symbol = "BTC_USDT";
    ctx.config.market_type = MarketType::Futures;
    m_strategyMgr.registerStrategy(std::make_unique<MockScalper>(ctx));

    // Default: auto_trade = 1.0 → snapshot shows trading on.
    auto snap = m_services->strategies();
    ASSERT_TRUE(snap.is_array());
    ASSERT_EQ(1u, snap.size());
    EXPECT_TRUE(snap[0]["auto_trade"].get<bool>());

    // One-click OFF → signals-only (board visible, no orders).
    EXPECT_TRUE(m_services->setStrategyTrading("mock_scalper_BTC_USDT", false));
    snap = m_services->strategies();
    ASSERT_EQ(1u, snap.size());
    EXPECT_FALSE(snap[0]["auto_trade"].get<bool>());

    // One-click ON → auto-trade restored.
    EXPECT_TRUE(m_services->setStrategyTrading("mock_scalper_BTC_USDT", true));
    snap = m_services->strategies();
    EXPECT_TRUE(snap[0]["auto_trade"].get<bool>());

    // Unknown strategy → false.
    EXPECT_FALSE(m_services->setStrategyTrading("no_such", true));
}

TEST_F(EngineServicesTest, SetStrategyTradingRecordsManualAudit)
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

    EXPECT_TRUE(svc.setStrategyTrading("mock_scalper_BTC_USDT", false));
    const auto snap = change_log.snapshot();
    ASSERT_EQ(1u, snap.size());
    EXPECT_EQ("manual", snap[0].source);
    EXPECT_EQ("mock_scalper_BTC_USDT", snap[0].strategy_id);
    EXPECT_EQ("auto_trade", snap[0].param_name);
    EXPECT_DOUBLE_EQ(1.0, snap[0].old_value); // default auto_trade
    EXPECT_DOUBLE_EQ(0.0, snap[0].new_value);
}

TEST_F(EngineServicesTest, SetStrategyTradingRegisteredInMethodRegistry)
{
    strategy::StrategyContext ctx;
    ctx.config.symbol = "BTC_USDT";
    ctx.config.market_type = MarketType::Futures;
    m_strategyMgr.registerStrategy(std::make_unique<MockScalper>(ctx));

    const auto reg = makeMethodRegistry(*m_services);
    const auto it = reg.find("set_strategy_trading");
    ASSERT_NE(reg.end(), it);

    const auto result = it->second(nlohmann::json{
        { "strategy_id", "mock_scalper_BTC_USDT" },
        { "enabled", false } });
    const auto payload = std::get<nlohmann::json>(result);
    ASSERT_TRUE(payload.is_array());
    ASSERT_EQ(1u, payload.size());
    EXPECT_FALSE(payload[0]["auto_trade"].get<bool>());

    // Non-boolean enabled → invalid request.
    const auto bad = it->second(nlohmann::json{
        { "strategy_id", "mock_scalper_BTC_USDT" },
        { "enabled", "yes" } });
    EXPECT_TRUE(std::holds_alternative<PulseError>(bad));
}
