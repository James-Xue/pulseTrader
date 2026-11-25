#pragma once
// IOrderTracker.hpp — Abstract order-lifecycle tracker (Layer 8 seam).
//
// Why this interface exists (2026-11-25): the control plane (OrderFlowExecutor,
// EngineServices) needs order-lifecycle feedback, but OrderTracker is concrete
// and bound to Gate (private WS + REST). Extracting only the methods that the
// control plane actually calls lets a paper-trading venue supply the same
// lifecycle in-process without touching L6/L7 or the 35-method control
// contract.
//
// Scope rule: this interface lists ONLY the methods used outside OrderTracker
// itself (verified by grep on 2026-11-25). Gate-specific extras such as
// findByClientIdPrefix() and the test-only simulation hooks stay on the
// concrete class so the interface never grows a Gate-shaped surface.
//
// Thread safety: implementations must be safe for concurrent calls from the
// control socket, the WS/REST tracker threads and the main loop. The
// completion callback may be invoked from any of those threads and must not
// be called while holding a lock that a callback would re-enter.

#include "core/PulseError.hpp"
#include "core/types.hpp"
#include "execution/ExecutionReport.hpp"

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pulse::execution
{

// ---------------------------------------------------------------------------
// OrderSnapshot — lightweight read-only snapshot of a tracked order.
//
// Used by the control plane (active orders list, grid service) to display
// active order state without exposing the tracker's internal record.
// ---------------------------------------------------------------------------
struct OrderSnapshot
{
    std::string order_id;
    std::string client_order_id; ///< M27: client-assigned id (e.g. "eth-grid-2100").
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
        , client_order_id{}
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
// IOrderTracker — lifecycle feedback contract for one market type.
// ---------------------------------------------------------------------------
class IOrderTracker
{
  public:
    /// Callback invoked once when an order reaches a terminal state
    /// (Filled or Cancelled). Rejections that never entered the book are
    /// reported as PulseError from the placer instead, not through here.
    using CompletionCallback = std::function<void(const ExecutionReport &)>;

    virtual ~IOrderTracker() = default;

    /// Begin tracking an order that the exchange accepted.
    ///
    /// Parameters:
    ///   1. order_id         — exchange-assigned order ID
    ///   2. symbol           — trading pair
    ///   3. side             — buy or sell
    ///   4. type             — market, limit, or post-only
    ///   5. requested_qty    — original order quantity
    ///   6. submit_mid_price — mid-price at submission time (slippage calc)
    ///   7. client_order_id  — optional client-assigned ID (strategy tracking)
    virtual void trackOrder(const std::string &order_id,
        const Symbol &symbol,
        Side side,
        OrderType type,
        Quantity requested_qty,
        Price submit_mid_price,
        const std::string &client_order_id = "")
        = 0;

    /// Current status of a tracked order, if it is still known.
    [[nodiscard]] virtual std::optional<OrderStatus>
    getStatus(const std::string &order_id) const = 0;

    /// Install the terminal-report sink. Called once during startup, before
    /// any order can complete.
    virtual void setCompletionCallback(CompletionCallback callback) = 0;

    /// Poll one order's status; returns the updated status or a PulseError.
    /// Implementations may also deliver a terminal report from this call —
    /// the immediate post-placement poll is the normal market-fill path.
    [[nodiscard]] virtual Result<OrderStatus>
    pollOrderStatus(const std::string &order_id) = 0;

    /// Poll every tracked order once (WS-loss safety net).
    virtual void reconcileAll() = 0;

    /// Same, holding `rest_mutex` around each individual status poll.
    /// The caller must NOT already hold the mutex (std::mutex is not
    /// recursive).
    virtual void reconcileAll(std::mutex &rest_mutex) = 0;

    /// Snapshot of all currently tracked (non-terminal) orders.
    [[nodiscard]] virtual std::vector<OrderSnapshot>
    activeOrders() const = 0;

    /// The N most recent terminal execution reports.
    [[nodiscard]] virtual std::vector<ExecutionReport>
    recentReports(std::size_t n = 20) const = 0;

    /// Stash a terminal report that never passed through trackOrder().
    ///
    /// Used for venues that close a position directly (TradFi CFD close
    /// endpoint, and the paper venue's stop/TP exits) so recentReports,
    /// trades.db and execution.log stay complete.
    virtual void recordCompletedReport(const ExecutionReport &report) = 0;

    /// Resolve a tracked (possibly synthetic) key to the real exchange id.
    /// Returns the input unchanged when no alias is recorded.
    [[nodiscard]] virtual std::string
    resolveExchangeId(const std::string &order_id) const = 0;
};

} // namespace pulse::execution
