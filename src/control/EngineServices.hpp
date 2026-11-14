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

#include "control/OrderIdempotency.hpp"
#include "core/ParamChangeLog.hpp"
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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::grid
{
class GridManager;
}

namespace pulse::risk
{
class GoldTrader;
}

namespace pulse::trade_recorder
{
class TradeRecorder;
}

namespace pulse::control
{

class OrderFlowExecutor;

class EngineServices
{
  public:
    /// Sink for order-lifecycle events pushed to control-socket sessions.
    ///
    /// `open_order` emits exactly one "submitted" event per order that
    /// actually reached the exchange — never for a rejected request and never
    /// for a replayed idempotent retry (nothing new was placed, so there is
    /// nothing to announce). The event is emitted from inside
    /// OrderFlowExecutor::placeManualOrder(), immediately after the exchange
    /// accepted the order and before the tracker's immediate fill poll, so a
    /// synchronously filled order always arrives as `submitted` → `completed`
    /// in that order. The sink receives an already-serialized event object;
    /// the transport-specific topic (symbol / market_type) is part of it, so
    /// this class stays free of any socket types.
    using OrderEventSink = std::function<void(const nlohmann::json &)>;

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
                   grid::GridManager *grid = nullptr,
                   core::ParamChangeLog *param_change_log = nullptr,
                   trade_recorder::TradeRecorder *trade_recorder = nullptr);

    // --- Queries (each returns ready-to-serialize JSON) ---
    [[nodiscard]] nlohmann::json status() const;

    /// Account snapshot (futures + spot + CFD).
    ///
    /// `include_spot_balances` also returns `spot_balances`, the per-currency
    /// spot breakdown. That breakdown may issue one public REST ticker call
    /// per unpriced currency while holding the shared REST mutex, so it is
    /// OFF by default: a caller that only needs the aggregate USDT balance
    /// (the polling UI, the REPL, MCP) should not pay for it.
    [[nodiscard]] nlohmann::json account(bool include_spot_balances = false);

    /// Snapshot the open positions, one entry per position with its
    /// `market_type` ("spot" | "futures" | "cfd") already serialized.
    ///
    /// reconcile runs the per-tracker `reconcileAll()` safety net (one REST
    /// status poll per tracked order) before snapshotting. It is throttled
    /// internally to kReconcileIntervalMs, so a fast UI poll (2 Hz) still
    /// produces at most one reconcile round per interval on the shared REST
    /// mutex. Pass false to skip the safety net entirely (tests, and callers
    /// that only need the in-memory view).
    [[nodiscard]] nlohmann::json positions(bool reconcile = true) const;

    /// Snapshot the active orders and recent fills.
    ///
    /// Every entry carries `market_type`; the three trackers are merged in
    /// spot → futures → cfd order. `reconcile` has the same throttled
    /// semantics as positions().
    [[nodiscard]] nlohmann::json orders(bool reconcile = true) const;
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

    /// Attach (or detach) the L7 gold trader for the control-plane status
    /// card. `trader` is null when the supervisor is disabled or failed its
    /// metadata check; the configuration and reason are still reported so the
    /// UI can explain why it is not armed.
    ///
    /// The pointer is read by control-session threads; call this during
    /// startup before the control socket accepts connections. The trader
    /// publishes a thread-safe JSON snapshot, so later calls are safe too.
    void setGoldTrader(const risk::GoldTrader *trader,
                       const GoldTraderConfig &config,
                       std::string disabled_reason);

    /// Attach the sink that receives `open_order` lifecycle events
    /// (subscribe_orders → order.update over the control socket).
    ///
    /// Call during startup, before the control socket accepts connections and
    /// before any order can be placed; the sink is read by every control
    /// session thread that calls openOrder(). A throwing sink is logged and
    /// swallowed — a push failure must never turn an accepted order into an
    /// error reply.
    void setOrderEventSink(OrderEventSink sink);

    /// Snapshot for the Gold Traders page status card.
    [[nodiscard]] nlohmann::json goldTraderStatus() const;
    [[nodiscard]] nlohmann::json market(const std::string &symbol,
                                        int book_levels,
                                        int klines,
                                        const std::string &market_type = "") const;

    // --- Commands ---

    /// Place a manual order — `open_order`.
    ///
    /// Idempotency contract (why: an external strategy runtime retries after
    /// a timeout, and a retry must never create a second exchange order):
    ///   1. With a non-empty `client_order_id`, the pair (client_order_id,
    ///      normalized order parameters) is the order identity. Repeating the
    ///      same identity never reaches the exchange twice: a finished first
    ///      attempt is replayed verbatim from the cache — the same
    ///      OrderResponse on success, the same PulseError on rejection — and
    ///      an attempt that is still running is rejected as duplicate.
    ///   2. Reusing a client_order_id with DIFFERENT parameters is rejected
    ///      (ControlInvalidRequest); it is never silently treated as the same
    ///      order, because the caller would then act on a reply that belongs
    ///      to a different order.
    ///   3. `size_authority: "caller"` orders must carry a client_order_id
    ///      (reduce_only closes are exempt: they are bounded by the open
    ///      position and may legitimately outlive it). Without a key there is
    ///      nothing to de-duplicate a retry against.
    ///   4. The cache is in-memory with a 600 s TTL (OrderIdempotency), so
    ///      the guarantee spans a client timeout, not an engine restart.
    ///   5. Requests without a client_order_id keep the historical
    ///      fire-and-forget behaviour: one call, one order.
    ///   6. Caller-authoritative opens are additionally governed by the
    ///      independent external-order gate ([control]
    ///      external_orders_enabled, see externalOrdersEnabled()). A rejected
    ///      request never reaches the idempotency cache, so enabling the gate
    ///      later lets the caller retry the same client_order_id.
    ///   7. Caller-authoritative non-reduce-only opens must also satisfy the
    ///      exchange-native protection floor, because PT software exits keep
    ///      out of an "external" position only while that floor exists: CFD
    ///      requires sl_price > 0, futures requires
    ///      [risk] exchange_stop_enabled = true (ensureFuturesStops()
    ///      attaches the catastrophe stop), and spot is refused outright.
    ///      Futures reduce_only closes are exempt (the exchange bounds them
    ///      by the open position). Like the gate, this check precedes the
    ///      cache, so a refusal does not consume the client_order_id.
    ///   8. A definitive rejection is replayed; a transport failure that may
    ///      have reached the exchange is replayed as ControlOrderOutcomeUnknown
    ///      (reconcile, never assume "not placed"); a pre-flight failure
    ///      (rate limit, halt, notional/position limits) leaves the key
    ///      retryable.
    [[nodiscard]] Result<execution::OrderResponse>
    openOrder(const nlohmann::json &params);

    /// Independent external-order gate — [control] external_orders_enabled.
    ///
    /// While false (fail-safe default) open_order rejects caller-authoritative
    /// opens (size_authority = "caller") with ControlInvalidRequest and the
    /// message names the TOML key. Exempt are engine-authority opens (manual
    /// REPL / MCP / UI one-shots) and FUTURES reduce_only closes, whose
    /// exchange-enforced flag bounds them by the open position. open_order
    /// rejects reduce_only for spot/CFD outright (the flag is never
    /// transmitted there), so it can never be used to bypass this gate.
    ///
    /// The flag is intentionally separate from [strategy] signal_only: PT
    /// strategies can stay signal-only while an external runtime trades, and
    /// `set_external_orders enabled=false` kills that runtime without an
    /// engine restart. Thread-safe (atomic); the store publishes nothing but
    /// the flag itself, so relaxed ordering suffices.
    [[nodiscard]] bool externalOrdersEnabled() const;
    void setExternalOrdersEnabled(bool enabled);

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

    /// Price tick (Gate `order_price_round`) for a futures contract.
    ///
    /// Trigger prices must be an integer multiple of this unit — Gate rejects
    /// anything else with 1009 AUTO_INVALID_PARAM_TRIGGER_PRICE (verified on
    /// testnet 2026-11-13). Returns 0 when the tick cannot be resolved; the
    /// caller must then skip placement rather than send a rejected price.
    ///
    /// The result is cached per contract. Main-thread only (the cache is not
    /// synchronized; the REST call itself takes rest_mutex internally).
    [[nodiscard]] double futuresPriceTick(const std::string &contract);

    /// Ensure every open FUTURES position has an exchange-side protective
    /// stop (audit B1 backfill).
    ///
    /// This is what covers positions the engine did not open itself — most
    /// importantly the ones imported by sync_positions after a restart,
    /// which never pass through the fill path.
    ///
    /// Behaviour:
    ///   1. No-op unless [risk] exchange_stop_enabled and futures REST wired.
    ///   2. A record whose next re-listing is not yet due is skipped without
    ///      REST calls.
    ///   3. An existing open trigger counts as the floor only when it closes
    ///      THIS position side on the loss side of THIS entry
    ///      (see execution::isProtectiveStopTrigger) — a take-profit or a
    ///      leftover stop from an earlier entry does not.
    ///   4. A recorded stop that vanished from the exchange listing is
    ///      dropped (logged at ERROR) and re-planned, so a cancelled
    ///      catastrophe trigger is not trusted forever.
    ///   5. The new trigger is rounded to the contract price unit. When no
    ///      safe stop can be expressed (the market already gapped through
    ///      the level, or the price unit is unknown) the position is left
    ///      OUT of the verified set and an ERROR is logged: the generic exit
    ///      manager keeps custody instead of the position being considered
    ///      covered.
    ///
    /// Returns the number of stops placed by this call.
    /// Main-thread only (the verified-stop cache is not synchronized).
    [[nodiscard]] int ensureFuturesStops();

    /// True when ensureFuturesStops() holds a verified exchange-side stop for
    /// this position id.
    ///
    /// This is the exit-ownership hand-off used by the ProtectiveExitManager
    /// filter in runTrade(): PT's software exits skip an external futures
    /// position only while this returns true, because the exchange is then
    /// the exit custodian. When no stop is proven — never placed, impossible
    /// to plan, or revalidation showed it cancelled — the generic exit
    /// manager keeps custody rather than leaving the position naked.
    /// Main-thread only (reads the ensureFuturesStops() cache).
    [[nodiscard]] bool hasExchangeStop(const std::string &position_id) const;

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
    /// Resume a paused grid (`grid_resume`) — Paused → Running; orders were
    /// kept during the pause, the next slow pass reconciles before acting.
    [[nodiscard]] Result<nlohmann::json> gridResume();
    /// Stop the grid (`grid_stop`) — cancels all eth-grid-* orders.
    [[nodiscard]] Result<nlohmann::json> gridStop();

    // --- AI tuning observability ---

    /// Parameter-change audit history, newest first (`get_param_history`).
    [[nodiscard]] nlohmann::json paramHistory() const;

    /// Per-strategy trade performance over the last `hours`
    /// (`get_strategy_performance`). Requires a wired TradeRecorder (SQLite).
    [[nodiscard]] Result<nlohmann::json> strategyPerformance(int hours) const;

    void haltTrading();
    void resumeTrading();
    bool pauseStrategy(const std::string &id);
    bool resumeStrategy(const std::string &id);

    /// One-click per-strategy execution gate (`set_strategy_trading`).
    ///
    /// enabled = false → the strategy's signals still publish to the signal
    /// board (get_signals) but never feed the aggregator, so no orders are
    /// placed (signals-only mode). enabled = true → signals participate in
    /// aggregation and may place orders through the normal risk gate.
    ///
    /// Independent of the active-market direction gate (a futures strategy
    /// still needs `switch futures` to actually trade while cfd is active)
    /// and of the manual order path (placeManualOrder is unaffected).
    /// Startup default comes from [strategy] signal_only (fail-safe: true →
    /// all signals-only). Recorded in the param audit log.
    ///
    /// Returns false when the strategy id is unknown.
    bool setStrategyTrading(const std::string &id, bool enabled);

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

    /// Best-effort USDT price for a spot currency.
    ///
    /// Order: USDT = 1.0, spot ticker cache, futures ticker cache, then one
    /// REST `getTicker(<CUR>_USDT)` whose result is cached for 60 s (or 300 s
    /// for a failed lookup). Returns 0.0 when no price can be obtained; the
    /// caller then reports the holding with value_usdt = null. Call only with
    /// m_restMutex held — the REST fallback needs the shared client.
    [[nodiscard]] double spotPriceUsdt(const std::string &currency);

    /// Shared REST serialization mutex (also used by the heartbeat logger).
    [[nodiscard]] std::mutex &restMutex();

  private:
    /// Serialize and push the `submitted` event for an order that
    /// OrderFlowExecutor::placeManualOrder() just placed.
    ///
    /// Runs synchronously on the control-session thread, inside the order
    /// flow and before the tracker's immediate fill poll. No-op without a
    /// sink; a throwing sink is logged and swallowed (the order is already
    /// placed on the exchange by then).
    void publishOrderSubmitted(const execution::OrderRequest &req,
                               const execution::OrderResponse &resp);

    [[nodiscard]] nlohmann::json strategyParamsJson(const strategy::StrategyParams &p) const;

    /// Feed pointer for a market type (nullptr if that market is not wired).
    [[nodiscard]] market::MarketFeed *feedFor(MarketType mt) const;

    /// Run the `reconcileAll()` safety net on all three trackers, at most
    /// once per kReconcileIntervalMs across every control connection.
    ///
    /// Why throttled: each reconcile issues one REST status poll per tracked
    /// order while the UI, the CLI and MCP may poll get_positions/get_orders
    /// independently. At the client's 2 Hz cadence, an unthrottled reconcile
    /// would flood the exchange with status polls. A single-flight flag makes
    /// a round that outlives the interval non-reentrant.
    ///
    /// The tracker holds m_restMutex around each individual status poll:
    /// GateRestClient is not thread-safe, so reconcile traffic must not
    /// overlap order placement or the account snapshot, but the critical
    /// section stays one HTTP request long instead of one round (which grows
    /// with the tracked-order count).
    ///
    /// The interval is measured from the END of the previous round, so a
    /// round that takes longer than the interval does not run back-to-back.
    void reconcileTrackersThrottled() const;

    std::string m_version;
    std::chrono::steady_clock::time_point m_engineStart;
    PulseConfig m_cfg;

    /// [control] external_orders_enabled — operator kill switch for the
    /// external (caller-sized) open path. Atomic because control sessions
    /// toggle it while order sessions read it; relaxed is enough because the
    /// flag publishes no other state.
    std::atomic<bool> m_externalOrdersEnabled;

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
    const risk::GoldTrader *m_goldTrader{ nullptr }; ///< L7 supervisor (nullable).
    GoldTraderConfig m_goldConfig;                   ///< Last configured thresholds.
    std::string m_goldDisabledReason;                ///< Why it is not armed (empty if armed).
    /// Push sink for `open_order` events (nullable; see setOrderEventSink).
    OrderEventSink m_orderEventSink;
    strategy::SignalBoard &m_signalBoard;
    std::mutex &m_restMutex;
    core::ParamChangeLog *m_paramChangeLog{ nullptr }; ///< Audit (nullable).
    trade_recorder::TradeRecorder *m_tradeRecorder{ nullptr }; ///< Stats (nullable).

    /// Display timezone for human-readable *_str timestamps in JSON output
    /// (from [control] display_timezone; default = machine local time).
    pulse::DisplayTimezone m_displayTz;

    /// Futures contract registry (quanto multiplier lookup for the futures
    /// position sync). May be null — sync then assumes quanto 1.0.
    std::shared_ptr<market::SymbolRegistry> m_registry;

    /// Per-currency REST price cache for `spot_balances` (see spotPriceUsdt).
    struct SpotPrice
    {
        double price{ 0.0 };
        std::chrono::steady_clock::time_point fetched{};
    };
    std::unordered_map<std::string, SpotPrice> m_spotPrices;
    std::unordered_map<std::string, double> m_priceTicks; ///< contract → order_price_round

    /// One exchange-native catastrophe stop this engine has verified on the
    /// exchange: the trigger order id (for diagnostics when it disappears)
    /// and when the next re-listing is due.
    struct ExchangeStopRecord
    {
        std::string trigger_order_id;
        std::chrono::steady_clock::time_point next_reverify;
    };

    /// position_id → verified catastrophe stop. Grows when ensureFuturesStops()
    /// places or adopts a trigger; re-listed when next_reverify falls due, so
    /// a cancelled stop drops out instead of being trusted forever.
    /// Main-thread only.
    std::unordered_map<std::string, ExchangeStopRecord> m_exchangeStops;

    /// How long a recorded stop is trusted without re-listing the contract.
    /// 30 s bounds the exposure of a stop cancelled out-of-band (operator in
    /// the Gate app, another tool) while keeping the REST cost at two listings
    /// per contract per minute.
    static constexpr std::chrono::seconds kExchangeStopReverifyInterval{ 30 };

    /// open_order duplicate suppression keyed by client_order_id. Lives for
    /// the engine's lifetime; see the contract on openOrder() and the state
    /// machine in OrderIdempotency.hpp.
    OrderIdempotency m_orderIdempotency;

    /// Budget for REST lookups inside one account() call, so a wallet with
    /// many unpriced altcoins cannot turn one snapshot into dozens of calls.
    std::size_t m_spotPriceLookups{ 0 };

    /// Epoch milliseconds when the last tracker reconcile round FINISHED.
    /// Mutable because the snapshot methods are const while
    /// reconcileTrackersThrottled() is a deliberate (bounded) side effect.
    mutable std::atomic<std::int64_t> m_lastReconcileMs{ 0 };

    /// True while a reconcile round is running. The interval check alone is a
    /// start-time rate limiter; this flag makes the round single-flight so a
    /// round slower than the interval cannot be re-entered by the next poll.
    mutable std::atomic<bool> m_reconcileRunning{ false };

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
