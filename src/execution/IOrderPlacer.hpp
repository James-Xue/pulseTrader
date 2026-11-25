#pragma once
// IOrderPlacer.hpp — Abstract order-placement seam (Layer 8).
//
// Why this interface exists (2026-11-25): OrderFlowExecutor dispatches every
// order through a placer pointer, but the interface was declared inside the
// control-layer header and its only implementation (ExecutorOrderPlacer) is a
// concrete Gate REST adapter. Moving the interface into L8 gives the paper
// venue (and unit tests) a seam that does not depend on pulse_control.
//
// The Gate implementation stays next to the control-plane wiring in
// OrderFlowExecutor.hpp; this header intentionally declares only the contract.
//
// Thread safety: implementations are called from the strategy/aggregator
// thread, the control socket and the main loop; they must be thread-safe.
// A placer must NEVER invoke the tracker completion callback synchronously
// from place(): the order reservation is inserted after place() returns.

#include "core/PulseError.hpp"
#include "execution/OrderExecutor.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace pulse::execution
{

class IOrderPlacer
{
  public:
    virtual ~IOrderPlacer() = default;

    /// Place an order.
    ///
    /// Returns the exchange acknowledgment (order id, accepted status) or a
    /// PulseError. A rejection that means "the order never entered the book"
    /// must be returned here as an error — not as a terminal Rejected report,
    /// because the order-flow reservation is released from this path.
    [[nodiscard]] virtual Result<OrderResponse>
    place(const OrderRequest &req) = 0;

    /// Cancel an order by exchange order ID.
    [[nodiscard]] virtual bool cancel(const std::string &order_id) = 0;

    /// Ensure futures leverage matches the request before an order is placed.
    /// Gate.io applies leverage at the position level, not per order — the
    /// account's current setting (possibly 200x) silently applies otherwise.
    /// Spot placers implement as a no-op.
    [[nodiscard]] virtual Result<nlohmann::json>
    setLeverage(const std::string &contract, double leverage) = 0;
};

} // namespace pulse::execution
