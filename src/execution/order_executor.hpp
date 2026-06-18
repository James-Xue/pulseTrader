#pragma once
// order_executor.hpp — REST order placement with retry (Layer 8 Order Execution)
//
// Submits orders via GateRestClient and handles transient failures with
// exponential backoff retry logic.

#include "core/error.hpp"
#include "core/types.hpp"
#include "exchange/gate_rest_client.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace pulse::execution
{

// ---------------------------------------------------------------------------
// OrderRequest — parameters for placing an order
// ---------------------------------------------------------------------------
struct OrderRequest
{
    Symbol symbol;              ///< Trading pair (e.g. "BTC_USDT").
    Side side;                  ///< Buy or Sell.
    OrderType type;             ///< Market, Limit, or PostOnly.
    Quantity quantity;          ///< Order quantity in base currency.
    Price price;                ///< Limit price (required for Limit/PostOnly, ignored for Market).
    std::string client_order_id; ///< Optional client-assigned ID for tracking.

    /// Default constructor.
    OrderRequest()
        : symbol{}
        , side{ Side::Buy }
        , type{ OrderType::Limit }
        , quantity{ 0.0 }
        , price{ 0.0 }
        , client_order_id{}
    {
    }
};

// ---------------------------------------------------------------------------
// OrderResponse — result of order placement
// ---------------------------------------------------------------------------
struct OrderResponse
{
    std::string order_id;       ///< Exchange-assigned order ID.
    OrderStatus status;         ///< Initial status (typically Open or Pending).
    Timestamp submit_time;      ///< When the order was submitted.

    /// Default constructor.
    OrderResponse()
        : order_id{}
        , status{ OrderStatus::Pending }
        , submit_time{}
    {
    }
};

// ---------------------------------------------------------------------------
// OrderExecutor — places orders via REST with retry logic
// ---------------------------------------------------------------------------
class OrderExecutor
{
  public:
    /// Construct an OrderExecutor with a reference to the REST client.
    explicit OrderExecutor(exchange::GateRestClient &rest_client);

    /// Place an order on Gate.io.
    ///
    /// Sends POST /api/v4/spot/orders with the given parameters.
    /// Retries up to 3 times on transient failures (5xx, timeout).
    ///
    /// Returns OrderResponse with order_id on success, or PulseError on failure.
    [[nodiscard]] Result<OrderResponse> place_order(const OrderRequest &req);

    /// Cancel an order by order_id.
    ///
    /// Sends DELETE /api/v4/spot/orders/{order_id}.
    /// Returns true on success, false on failure (check logs for details).
    [[nodiscard]] bool cancel_order(const std::string &order_id);

  private:
    exchange::GateRestClient &rest_client_;

    /// Build Gate.io order JSON body from OrderRequest.
    ///
    /// Gate.io format:
    /// {
    ///   "currency_pair": "BTC_USDT",
    ///   "type": "limit",
    ///   "side": "buy",
    ///   "amount": "0.001",
    ///   "price": "50000",
    ///   "time_in_force": "gtc"
    /// }
    [[nodiscard]] nlohmann::json build_order_body(const OrderRequest &req) const;

    /// Parse Gate.io order response JSON into OrderResponse.
    ///
    /// Gate.io response format:
    /// {
    ///   "id": "12345",
    ///   "status": "open",
    ///   "currency_pair": "BTC_USDT",
    ///   ...
    /// }
    [[nodiscard]] OrderResponse parse_order_response(const nlohmann::json &resp) const;
};

} // namespace pulse::execution
