#pragma once
// igrid_gateway.hpp — GridManager's exchange-facing abstraction (M27)
//
// Decouples the grid state machine from the control-plane order path: the
// pulse_grid library must not depend on pulse_control (EngineServices owns a
// GridManager, so a direct dependency would be a link cycle). The production
// implementation (GridGateway, assembled in main.cpp) forwards to
// OrderFlowExecutor::placeManualOrder (full risk gate) and the futures REST
// client. Tests inject a fake.
//
// The exchange is the source of truth for open orders (list_futures_orders):
// the engine tracker loses state on restart, the exchange does not.

#include "core/PulseError.hpp"
#include "execution/OrderExecutor.hpp"

#include <string>
#include <vector>

namespace pulse::grid
{

/// Lightweight view of one exchange futures order (list_futures_orders item).
struct ExchangeOrderView
{
    std::string order_id;        ///< Exchange order id.
    std::string client_order_id; ///< Client-assigned id (e.g. "eth-grid-2100").
    double price{ 0.0 };
    double quantity{ 0.0 };      ///< Signed (negative = sell).
    bool reduce_only{ false };
    std::string status;          ///< "open" / "finished" / ...
};

/// Lightweight view of one futures position (user-share warning only).
struct PositionView
{
    std::string symbol;
    std::string strategy_id;     ///< Empty for user/externally-opened positions.
    double quantity{ 0.0 };      ///< Signed (negative = short).
    double entry_price{ 0.0 };
    double notional_value{ 0.0 };
    bool is_short{ false };
};

/// Exchange-facing operations the grid needs. All methods must be safe to
/// call from the main-loop tick thread.
class IGridGateway
{
  public:
    virtual ~IGridGateway() = default;

    /// Place an order through the FULL risk gate (evaluate → reserve →
    /// execute → track). Returns the exchange order id on success.
    [[nodiscard]] virtual Result<execution::OrderResponse>
    place(const execution::OrderRequest &req) = 0;

    /// Cancel one open order by exchange id. True when the cancel was
    /// accepted (or the order was already gone).
    [[nodiscard]] virtual bool cancel(const std::string &order_id) = 0;

    /// All open futures orders for a contract, straight from the exchange
    /// (never the engine tracker — it loses state on restart).
    [[nodiscard]] virtual std::vector<ExchangeOrderView>
    openFuturesOrders(const std::string &contract) = 0;

    /// Open futures positions for a symbol (user-share coexistence warning).
    [[nodiscard]] virtual std::vector<PositionView>
    positionsBySymbol(const std::string &symbol) = 0;
};

} // namespace pulse::grid
