#pragma once
// order_tracker.hpp — WS + REST order tracking (Layer 8 Order Execution)
//
// Tracks order lifecycle via WebSocket private channel (spot.orders) with
// REST polling fallback. Generates ExecutionReport when order reaches
// terminal state (Filled or Cancelled).

#include "core/types.hpp"
#include "execution/ExecutionReport.hpp"
#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::execution
{

// ---------------------------------------------------------------------------
// OrderSnapshot — lightweight read-only snapshot of a tracked order
//
// Used by the WebUI dashboard (Layer 9) to display active order state without
// exposing internal TrackedOrder struct.
// ---------------------------------------------------------------------------
struct OrderSnapshot
{
    std::string order_id;
    Symbol symbol;
    Side side;
    OrderType type;
    Quantity requested_qty;
    Quantity filled_qty;
    OrderStatus status;
    Timestamp submit_time;
    Timestamp last_update_time;

    OrderSnapshot()
        : order_id{}
        , symbol{}
        , side{ Side::Buy }
        , type{ OrderType::Market }
        , requested_qty{ 0.0 }
        , filled_qty{ 0.0 }
        , status{ OrderStatus::Pending }
        , submit_time{}
        , last_update_time{}
    {
    }
};

// ---------------------------------------------------------------------------
// OrderTracker — tracks orders via WS + REST fallback
// ---------------------------------------------------------------------------
class OrderTracker
{
  public:
    /// Callback type for order completion (Filled or Cancelled).
    using CompletionCallback = std::function<void(const ExecutionReport &)>;

    /// Construct an OrderTracker with WS and REST clients.
    ///
    /// market_type selects which WS channel and REST paths to use.
    /// ws_client may be nullptr when enable_ws=false (TradFi CFD has no
    /// private WS channel): the tracker is REST-poll-only, all status comes
    /// from pollOrderStatus / reconcileAll.
    OrderTracker(exchange::GateWsClient *ws_client, exchange::GateRestClient &rest_client,
                 MarketType market_type = MarketType::Spot, bool enable_ws = true);

    /// Start tracking an order.
    ///
    /// Subscribes to WS private channel (spot.orders) if not already subscribed.
    /// Stores order metadata for later ExecutionReport generation.
    ///
    /// Parameters:
    ///   1. order_id         — exchange-assigned order ID
    ///   2. symbol           — trading pair
    ///   3. side             — buy or sell
    ///   4. type             — market, limit, or post-only
    ///   5. requested_qty    — original order quantity
    ///   6. submit_mid_price — mid-price at submission time (for slippage calc)
    ///   7. client_order_id  — optional client-assigned ID (strategy tracking)
    void trackOrder(const std::string &order_id,
        const Symbol &symbol,
        Side side,
        OrderType type,
        Quantity requested_qty,
        Price submit_mid_price,
        const std::string &client_order_id = "");

    /// Stop tracking an order (reached terminal state).
    void stopTracking(const std::string &order_id);

    /// Get current order status.
    [[nodiscard]] std::optional<OrderStatus> getStatus(const std::string &order_id) const;

    /// Get execution report (only available for terminal states).
    [[nodiscard]] std::optional<ExecutionReport> getReport(const std::string &order_id) const;

    /// Set callback invoked when an order reaches terminal state.
    void setCompletionCallback(CompletionCallback callback);

    /// Poll order status via REST (fallback when WS events are missed).
    ///
    /// Calls GET /api/v4/spot/orders/{order_id} and updates internal state.
    /// Returns the updated status, or PulseError on failure.
    [[nodiscard]] Result<OrderStatus> pollOrderStatus(const std::string &order_id);

    /// Poll every tracked order's status via REST (reconcile fallback).
    ///
    /// Catches fills that arrived before the WS private-channel subscription
    /// was established (market orders can fill within the same millisecond).
    /// Individual failures are logged and tolerated; terminal orders emit
    /// completion reports through the normal path.
    void reconcileAll();

    /// Check if an order status is terminal (Filled or Cancelled).
    [[nodiscard]] static bool isTerminalStatus(OrderStatus status);

    /// Parse order status string from Gate.io API.
    [[nodiscard]] static OrderStatus parseStatus(const std::string &status_str);

    /// Parse a futures order status from Gate.io API (status + finish_as).
    ///
    /// Futures uses status="open"/"finished" plus a separate finish_as
    /// ("filled"/"cancelled"/"reduce_only"/"position_closed") that decides
    /// the terminal outcome — status alone is ambiguous for a finished order.
    [[nodiscard]] static OrderStatus parseFuturesStatus(const std::string &status,
                                                        const std::string &finish_as);

    /// Parse a TradFi CFD order object into an OrderStatus.
    ///
    /// CFD order objects use `state` (int) + `finished` (0/1) instead of the
    /// spot/futures `status` string (probe-verified 2026-08-17). Mapping:
    ///   finished != 0  → terminal; state == 1 → Cancelled (deleted),
    ///                    otherwise → Filled
    ///   finished == 0  → state >= 1 ? Open : Pending
    /// Pure function — unit-tested.
    [[nodiscard]] static OrderStatus parseCfdOrderStatus(const nlohmann::json &order_obj);

    /// Find an order by id inside a TradFi open-orders list (`data.list`).
    ///
    /// order_id may be encoded as int or string in the list. Returns the
    /// matching order object (a reference into the list), or a null JSON
    /// value when not found or the list is not an array. Pure function.
    [[nodiscard]] static nlohmann::json findCfdOrderInList(const nlohmann::json &list,
                                                           const std::string &order_id);

    /// Key-match fallback: find a TradFi order by symbol/side/volume/price
    /// within the submit-time window, NOT by id.
    ///
    /// Used when the engine tracks a POST response's data.id while the list
    /// carries the real exchange order id (list read lagged the POST —
    /// 2026-08-17: trigger 47777 never matched by id, mis-cancelled while
    /// 17654490 was still open). Market orders match on symbol/side/volume;
    /// trigger orders additionally require price equality. Pure function.
    [[nodiscard]] static nlohmann::json findCfdOrderByKey(
        const nlohmann::json &list, const Symbol &symbol, Side side,
        Quantity qty, OrderType type, Price price, std::int64_t submit_sec);

    /// Returns a snapshot of all currently tracked (non-terminal) orders.
    /// Thread-safe: takes shared read lock.
    [[nodiscard]] std::vector<OrderSnapshot> activeOrders() const;

    /// Returns the N most recent execution reports (completed orders).
    /// Thread-safe: takes shared read lock.
    ///
    /// Parameters:
    ///   1. n — maximum number of reports to return (default: 20)
    [[nodiscard]] std::vector<ExecutionReport> recentReports(std::size_t n = 20) const;

  public:
    /// Test-only: simulate a WS order update event (calls processOrderUpdate).
    /// Allows unit tests to exercise the state machine without a real WS connection.
    void testSimulateWsUpdate(const nlohmann::json &event)
    {
        processOrderUpdate(event);
    }

    /// Resolve a tracked (possibly data.id) key to the real exchange order id.
    ///
    /// TradFi CFD: POST returns data.id, the open-orders list carries the
    /// real id. After pollCfdOrderStatus re-anchors a tracked key, this
    /// returns the real id (callers like cancel need it). Returns the input
    /// unchanged when no alias is recorded.
    [[nodiscard]] std::string resolveExchangeId(const std::string &order_id) const;

    /// Test-only: simulate a CFD list-poll result for one order (calls the
    /// same apply+complete path as pollCfdOrderStatus, without network).
    void testSimulateCfdPoll(const nlohmann::json &order_obj);

    /// Test-only: try to acquire a shared read lock (returns immediately).
    /// Used to verify that callbacks run outside the write lock.
    [[nodiscard]] bool testTrySharedLock()
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex, std::try_to_lock);
        return lock.owns_lock();
    }

  private:
    exchange::GateWsClient *m_wsClient; ///< Null for REST-poll-only markets (CFD).
    exchange::GateRestClient &m_restClient;
    MarketType m_marketType;

    /// Internal state for a tracked order.
    struct TrackedOrder
    {
        std::string order_id;
        std::string client_order_id;
        Symbol symbol;
        Side side;
        OrderType type;
        Quantity requested_qty;
        Price submit_mid_price;
        Quantity filled_qty;
        Price avg_fill_price;
        Price fees;
        OrderStatus status;
        Timestamp submit_time;
        Timestamp last_update_time;
        std::string exchange_position_id; ///< TradFi CFD only — exchange-side
                                          ///< position id from the fill ("" elsewhere).
    };

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, TrackedOrder> m_trackedOrders;
    std::unordered_map<std::string, ExecutionReport> m_completedReports;
    /// data.id → real exchange id (TradFi CFD re-anchor map). A POST returns
    /// the internal submission number; the open-orders list carries the real
    /// id, so the tracked key may be re-anchored by pollCfdOrderStatus. The
    /// alias lets callers (cancel) resolve the tracked key to the real id.
    std::unordered_map<std::string, std::string> m_idAliases;
    CompletionCallback m_completionCallback;
    bool m_wsSubscribed; ///< Whether we've subscribed to the orders channel.
    bool m_enableWs;     ///< False = REST-poll-only (TradFi CFD has no WS channel).

    /// Poll a CFD order's status via the open-orders list (the single-order
    /// GET /tradfi/orders/{id} endpoint does not exist — route-level 404).
    /// Falls back to position-based fill detection when the order has left
    /// the list (a filled market order disappears immediately).
    [[nodiscard]] Result<OrderStatus> pollCfdOrderStatus(const std::string &order_id);

    /// WS callback for spot.orders channel events.
    void onOrderUpdate(const nlohmann::json &event);

    /// Parse WS order update event and update tracked order state.
    void processOrderUpdate(const nlohmann::json &event);

    /// Shared lock-held state machine: apply an order object update to a
    /// tracked order and, on terminal state, generate the report + stash the
    /// callback for out-of-lock invocation. Returns the report if completed.
    std::optional<ExecutionReport> applyUpdateAndMaybeComplete(const std::string &order_id,
                                                               const nlohmann::json &update);

    /// Apply a Gate.io order object (WS event result or REST response) to a
    /// tracked order. Handles both spot and futures field layouts.
    void applyOrderUpdate(const nlohmann::json &update, TrackedOrder &order);

    /// Generate ExecutionReport when order reaches terminal state.
    [[nodiscard]] ExecutionReport generateReport(const TrackedOrder &order, Timestamp fill_time) const;
};

} // namespace pulse::execution
