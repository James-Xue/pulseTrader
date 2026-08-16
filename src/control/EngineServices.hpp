#pragma once
// EngineServices.hpp — Thread-safe service bundle for the control plane
//
// Every method returns ready-to-serialize nlohmann::json (or a Result for
// order operations). All REST-touching calls (account, open/close/cancel)
// serialize through the shared rest_mutex (GateRestClient is documented
// NOT thread-safe).
//
// All component accessors are internally synchronized (shared_mutex /
// atomics / seqlock), so concurrent control sessions are safe.

#include "core/PulseError.hpp"
#include "core/TimeUtil.hpp"
#include "core/config.hpp"
#include "exchange/GateRestClient.hpp"
#include "execution/OrderExecutor.hpp"
#include "execution/OrderTracker.hpp"
#include "market/MarketFeed.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/StrategyManager.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace pulse::control
{

class OrderFlowExecutor;

class EngineServices
{
  public:
    EngineServices(std::string version,
                   std::chrono::steady_clock::time_point engine_start,
                   const PulseConfig &cfg,
                   strategy::StrategyManager &strategy_mgr,
                   risk::RiskManager &risk_mgr,
                   risk::PositionManager &position_mgr,
                   market::MarketFeed *spot_feed,
                   market::MarketFeed *futures_feed,
                   market::MarketFeed *cfd_feed,
                   exchange::GateRestClient *spot_rest,
                   exchange::GateRestClient *futures_rest,
                   exchange::GateRestClient *cfd_rest,
                   execution::OrderTracker *spot_tracker,
                   execution::OrderTracker *futures_tracker,
                   execution::OrderTracker *cfd_tracker,
                   OrderFlowExecutor &order_flow,
                   std::mutex &rest_mutex);

    // --- Queries (each returns ready-to-serialize JSON) ---
    [[nodiscard]] nlohmann::json status() const;
    [[nodiscard]] nlohmann::json account();
    [[nodiscard]] nlohmann::json positions() const;
    [[nodiscard]] nlohmann::json orders() const;
    [[nodiscard]] nlohmann::json strategies() const;
    [[nodiscard]] nlohmann::json getStrategyParams(const std::string &id) const;
    [[nodiscard]] bool setStrategyParam(const std::string &id,
                                        const std::string &param,
                                        double value);
    [[nodiscard]] nlohmann::json risk() const;
    [[nodiscard]] nlohmann::json market(const std::string &symbol,
                                        int book_levels,
                                        int klines,
                                        const std::string &market_type = "") const;

    // --- Commands ---
    [[nodiscard]] Result<execution::OrderResponse>
    openOrder(const nlohmann::json &params);

    [[nodiscard]] Result<execution::OrderResponse>
    closePosition(const nlohmann::json &params);

    /// Cancel an open order; probes futures tracker first, then spot.
    [[nodiscard]] bool cancelOrder(const std::string &order_id);

    void haltTrading();
    void resumeTrading();
    bool pauseStrategy(const std::string &id);
    bool resumeStrategy(const std::string &id);

    /// Switch the single active trading direction.
    ///
    /// Sequence (under rest_mutex): close the order-flow gate for the new
    /// direction first, pause every strategy of the old direction, resume
    /// every strategy of the new direction, then cancel the old direction's
    /// open orders. Open POSITIONS stay open (closeable manually via
    /// close_position). The switch is ephemeral — a restart returns to
    /// config's `active_market`.
    ///
    /// Returns the status() JSON augmented with switched_from / switched_to /
    /// strategies_resumed / cancelled_orders (or an error object).
    [[nodiscard]] nlohmann::json switchDirection(const std::string &direction);

    /// Shared REST serialization mutex (also used by the heartbeat logger).
    [[nodiscard]] std::mutex &restMutex();

  private:
    [[nodiscard]] nlohmann::json strategyParamsJson(const strategy::StrategyParams &p) const;

    /// Feed pointer for a market type (nullptr if that market is not wired).
    [[nodiscard]] market::MarketFeed *feedFor(MarketType mt) const;

    std::string m_version;
    std::chrono::steady_clock::time_point m_engineStart;
    PulseConfig m_cfg;
    strategy::StrategyManager &m_strategyMgr;
    risk::RiskManager &m_riskMgr;
    risk::PositionManager &m_positionMgr;
    market::MarketFeed *m_spotFeed;
    market::MarketFeed *m_futuresFeed;
    market::MarketFeed *m_cfdFeed;
    exchange::GateRestClient *m_spotRest;
    exchange::GateRestClient *m_futuresRest;
    exchange::GateRestClient *m_cfdRest;
    execution::OrderTracker *m_spotTracker;
    execution::OrderTracker *m_futuresTracker;
    execution::OrderTracker *m_cfdTracker;
    OrderFlowExecutor &m_orderFlow;
    std::mutex &m_restMutex;

    /// Display timezone for human-readable *_str timestamps in JSON output
    /// (from [control] display_timezone; default = machine local time).
    pulse::DisplayTimezone m_displayTz;
};

} // namespace pulse::control
