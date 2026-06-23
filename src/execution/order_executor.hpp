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

    // Futures-specific fields (defaults make spot orders work unchanged).
    MarketType market_type;     ///< Spot or Futures (default Spot).
    double leverage;            ///< Leverage multiplier (futures only, default 1.0).
    bool reduce_only;           ///< Reduce-only flag (futures only, default false).
    int contract_size;          ///< Order size in contracts (futures only, 0 = use quantity).

    /// Default constructor.
    OrderRequest()
        : symbol{}
        , side{ Side::Buy }
        , type{ OrderType::Limit }
        , quantity{ 0.0 }
        , price{ 0.0 }
        , client_order_id{}
        , market_type{ MarketType::Spot }
        , leverage{ 1.0 }
        , reduce_only{ false }
        , contract_size{ 0 }
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
    ///
    /// market_type selects spot or futures order endpoints and body format.
    explicit OrderExecutor(exchange::GateRestClient &rest_client,
                           MarketType market_type = MarketType::Spot);

    /// Place an order on Gate.io.
    ///
    /// Spot:    POST /api/v4/spot/orders
    /// Futures: POST /api/v4/futures/usdt/orders
    /// Retries up to 3 times on transient failures (5xx, timeout).
    ///
    /// Returns OrderResponse with order_id on success, or PulseError on failure.
    [[nodiscard]] Result<OrderResponse> placeOrder(const OrderRequest &req);

    /// Cancel an order by order_id.
    ///
    /// Spot:    DELETE /api/v4/spot/orders/{order_id}
    /// Futures: DELETE /api/v4/futures/usdt/orders/{order_id}
    /// Returns true on success, false on failure (check logs for details).
    [[nodiscard]] bool cancelOrder(const std::string &order_id);

  private:
    exchange::GateRestClient &m_restClient;
    MarketType m_marketType;

    /// Build Gate.io order JSON body from OrderRequest.
    ///
    /// Spot format:
    /// {
    ///   "currency_pair": "BTC_USDT",
    ///   "type": "limit",
    ///   "side": "buy",
    ///   "amount": "0.001",
    ///   "price": "50000",
    ///   "time_in_force": "gtc"
    /// }
    ///
    /// Futures format:
    /// {
    ///   "contract": "BTC_USDT",
    ///   "size": 100,
    ///   "price": "50000",
    ///   "tif": "gtc",
    ///   "reduce_only": false
    /// }
    [[nodiscard]] nlohmann::json buildOrderBody(const OrderRequest &req) const;

    /// Parse Gate.io order response JSON into OrderResponse.
    ///
    /// Spot:    "id" (string), "status" (open/closed/cancelled)
    /// Futures: "id" (integer), "status" (open/finished), "finish_as" (filled/cancelled)
    [[nodiscard]] OrderResponse parseOrderResponse(const nlohmann::json &resp) const;
};

} // namespace pulse::execution
