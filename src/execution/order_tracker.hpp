#pragma once
// order_tracker.hpp — WS + REST order tracking (Layer 8 Order Execution)
//
// Tracks order lifecycle via WebSocket private channel (spot.orders) with
// REST polling fallback. Generates ExecutionReport when order reaches
// terminal state (Filled or Cancelled).

#include "core/types.hpp"
#include "execution/execution_report.hpp"
#include "exchange/gate_rest_client.hpp"
#include "exchange/gate_ws_client.hpp"

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
    OrderTracker(exchange::GateWsClient &ws_client, exchange::GateRestClient &rest_client,
                 MarketType market_type = MarketType::Spot);

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
    void track_order(const std::string &order_id,
        const Symbol &symbol,
        Side side,
        OrderType type,
        Quantity requested_qty,
        Price submit_mid_price,
        const std::string &client_order_id = "");

    /// Stop tracking an order (reached terminal state).
    void stop_tracking(const std::string &order_id);

    /// Get current order status.
    [[nodiscard]] std::optional<OrderStatus> get_status(const std::string &order_id) const;

    /// Get execution report (only available for terminal states).
    [[nodiscard]] std::optional<ExecutionReport> get_report(const std::string &order_id) const;

    /// Set callback invoked when an order reaches terminal state.
    void set_completion_callback(CompletionCallback callback);

    /// Poll order status via REST (fallback when WS events are missed).
    ///
    /// Calls GET /api/v4/spot/orders/{order_id} and updates internal state.
    /// Returns the updated status, or PulseError on failure.
    [[nodiscard]] Result<OrderStatus> poll_order_status(const std::string &order_id);

    /// Check if an order status is terminal (Filled or Cancelled).
    [[nodiscard]] static bool is_terminal_status(OrderStatus status);

    /// Parse order status string from Gate.io API.
    [[nodiscard]] static OrderStatus parse_status(const std::string &status_str);

    /// Returns a snapshot of all currently tracked (non-terminal) orders.
    /// Thread-safe: takes shared read lock.
    [[nodiscard]] std::vector<OrderSnapshot> active_orders() const;

    /// Returns the N most recent execution reports (completed orders).
    /// Thread-safe: takes shared read lock.
    ///
    /// Parameters:
    ///   1. n — maximum number of reports to return (default: 20)
    [[nodiscard]] std::vector<ExecutionReport> recent_reports(std::size_t n = 20) const;

  private:
    exchange::GateWsClient &ws_client_;
    exchange::GateRestClient &rest_client_;
    MarketType market_type_;

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
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, TrackedOrder> tracked_orders_;
    std::unordered_map<std::string, ExecutionReport> completed_reports_;
    CompletionCallback completion_callback_;
    bool ws_subscribed_; ///< Whether we've subscribed to spot.orders channel.

    /// WS callback for spot.orders channel events.
    void on_order_update(const nlohmann::json &event);

    /// Parse WS order update event and update tracked order state.
    void process_order_update(const nlohmann::json &event);

    /// Generate ExecutionReport when order reaches terminal state.
    [[nodiscard]] ExecutionReport generate_report(const TrackedOrder &order, Timestamp fill_time) const;
};

} // namespace pulse::execution
