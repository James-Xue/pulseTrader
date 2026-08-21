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
#include "market/SymbolRegistry.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/StrategyManager.hpp"
#include "strategy/signal/SignalBoard.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace pulse::grid
{
class GridManager;
}

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
                   strategy::SignalBoard &signal_board,
                   std::mutex &rest_mutex,
                   const std::shared_ptr<market::SymbolRegistry> &registry =
                       nullptr,
                   grid::GridManager *grid = nullptr);

    // --- Queries (each returns ready-to-serialize JSON) ---
    [[nodiscard]] nlohmann::json status() const;
    [[nodiscard]] nlohmann::json account();
    [[nodiscard]] nlohmann::json positions() const;
    [[nodiscard]] nlohmann::json orders() const;
    [[nodiscard]] nlohmann::json strategies() const;

    /// Latest per-strategy signals + indicator snapshots + aggregator
    /// consensus from the signal board (`get_signals`). Publish timestamps
    /// get a display-timezone `ts_str` companion field.
    [[nodiscard]] nlohmann::json signals() const;

    /// Reconcile the engine position view against the exchange (futures +
    /// CFD) — `sync_positions`. Imports missing positions and prunes local
    /// ghosts that no longer exist on the exchange (e.g. manual app-side
    /// closes). Runs at startup, on a ~10s background tick, and on demand.
    /// Never fatal: exchange failures return a summary with zeros.
    [[nodiscard]] nlohmann::json syncPositions();

    /// Dynamically adjust the protective stops on an open CFD position —
    /// `modify_sl_tp` (exchange-native price_sl/price_tp, "0" clears a stop).
    /// Params: position_id (required), sl_price?, tp_price?. CFD only.
    [[nodiscard]] Result<nlohmann::json> modifySlTp(const nlohmann::json &params);
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

    // --- Futures trigger orders (price_orders) — M23 ---
    //
    // The TP/SL attached to a futures position are INDEPENDENT trigger orders
    // on /futures/usdt/price_orders (2026-08-16 memo) — the position's
    // close_order field does not show them. These three methods expose
    // create/list/cancel so the SNDK grid sub-agent can manage per-slot
    // take-profits without the App.

    /// Create a futures trigger order — `place_trigger_order`.
    ///
    /// Params: contract (required), trigger_price (required), rule (1 = price
    /// crosses above → SL for shorts / TP for longs; 2 = price crosses below
    /// → TP for shorts / SL for longs; default 2), size (contracts to close,
    /// default 0), order_type (close-short-position / close-long-position,
    /// default close-short-position), tif (default "ioc"), auto_size (default
    /// "close"). Serialized through the shared REST mutex.
    [[nodiscard]] Result<nlohmann::json> placeTriggerOrder(const nlohmann::json &params);

    /// List open trigger orders for a contract — `list_trigger_orders`.
    /// Params: contract (required). Returns the raw exchange array.
    [[nodiscard]] Result<nlohmann::json> listTriggerOrders(const nlohmann::json &params);

    /// List ALL open futures orders for a contract straight from the exchange
    /// — `list_futures_orders`. Includes orders the engine did not place
    /// (App-side orders, pre-restart leftovers) that the tracker-based
    /// `orders()` view misses. Params: contract (required).
    [[nodiscard]] Result<nlohmann::json> listFuturesOrders(const nlohmann::json &params);

    /// Cancel a trigger order by ID — `cancel_trigger_order`.
    /// Params: order_id (required).
    [[nodiscard]] Result<nlohmann::json> cancelTriggerOrder(const nlohmann::json &params);

    // --- M27 grid service ---
    /// Start the engine-native grid (`grid_start`). Optional params:
    /// levels / qty_per_level / step / anchor (overrides, PR-4 formal).
    [[nodiscard]] Result<nlohmann::json> gridStart(
        const nlohmann::json &params);
    /// Full grid snapshot (`grid_status`).
    [[nodiscard]] nlohmann::json gridStatus() const;
    /// Pause the grid (`grid_pause`) — orders stay, no new action.
    [[nodiscard]] Result<nlohmann::json> gridPause();
    /// Stop the grid (`grid_stop`) — cancels all eth-grid-* orders.
    [[nodiscard]] Result<nlohmann::json> gridStop();

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
    grid::GridManager *m_grid{ nullptr }; ///< M27 grid service (nullable).
    strategy::SignalBoard &m_signalBoard;
    std::mutex &m_restMutex;

    /// Display timezone for human-readable *_str timestamps in JSON output
    /// (from [control] display_timezone; default = machine local time).
    pulse::DisplayTimezone m_displayTz;

    /// Futures contract registry (quanto multiplier lookup for the futures
    /// position sync). May be null — sync then assumes quanto 1.0.
    std::shared_ptr<market::SymbolRegistry> m_registry;

    /// Sync futures positions from the exchange (mirrors the old startup
    /// free function in main.cpp; consolidated here so startup, the ~10s
    /// background tick and the manual `sync_positions` share one code path).
    /// Returns the number of positions imported.
    [[nodiscard]] int syncFuturesPositionsFromExchange();

    /// Sync TradFi CFD positions from the exchange, then prune local ghosts.
    /// Returns the number of positions imported; the number of pruned ghosts
    /// is written to pruned_out when non-null.
    [[nodiscard]] int syncCfdPositionsFromExchange(int *pruned_out = nullptr);

    /// Remove local positions of a market type whose exchange_position_id
    /// is absent from the fresh exchange list and whose age exceeds the
    /// grace period (freshly-opened fills may not have appeared yet).
    /// Returns the number of positions pruned.
    [[nodiscard]] int pruneGhostPositions(
        MarketType mt, const std::vector<std::string> &live_exchange_ids);

    /// Close a position that vanished from the exchange with no local close
    /// record (manual app close, exchange-side stop/liquidation) and
    /// synthesize the external-close report so the audit trail (recent
    /// reports / trades.db / drawdown guard) stays complete. The exit price
    /// is a best-effort estimate from the last known mark. Returns 1 when
    /// the ghost was pruned.
    [[nodiscard]] int traceExternalGhostClose(const risk::Position &pos);

    /// Prune futures positions on contracts the exchange no longer holds.
    /// Futures carry no exchange_position_id (positions merge per contract),
    /// so the live contract set is the only handle — a contract with zero
    /// exchange size means every engine fill on it was closed externally
    /// (2026-08-20 ETH grid: user's app market order flattened the contract
    /// while 15 fill-tracked entries lingered with fake unrealized PnL).
    /// Same grace window as pruneGhostPositions.
    [[nodiscard]] int pruneGhostFuturesByContract(
        const std::set<std::string> &live_contracts);
};

} // namespace pulse::control
