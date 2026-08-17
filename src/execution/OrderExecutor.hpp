#pragma once
// order_executor.hpp — REST order placement with retry (Layer 8 Order Execution)
//
// Submits orders via GateRestClient and handles transient failures with
// exponential backoff retry logic.

#include "core/PulseError.hpp"
#include "core/types.hpp"
#include "exchange/GateRestClient.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

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
    double quanto_multiplier;   ///< Contract multiplier (e.g. 0.0001 = 1 contract = 0.0001 BTC).

    /// Default constructor.
    OrderRequest()
        : symbol{}
        , side{ Side::Buy }
        , type{ OrderType::Limit }
        , quantity{ 0.0 }
        , price{ 0.0 }
        , client_order_id{}
        , market_type{ MarketType::Spot }
        , leverage{ 0.0 }   ///< 0 = do not manage leverage (no API call)
        , reduce_only{ false }
        , contract_size{ 0 }
        , quanto_multiplier{ 1.0 }
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

    /// Set futures leverage for a contract before placing orders.
    ///
    /// Gate.io applies leverage at the position level, not per order — this
    /// must be called explicitly or the account's current setting (which may
    /// be 200x) silently applies. Cached per (contract, leverage) so repeated
    /// orders only hit the API once.
    ///
    /// Parameters:
    ///   1. contract — futures contract (e.g. "BTC_USDT")
    ///   2. leverage — leverage multiple (e.g. 10 for 10x cross margin)
    [[nodiscard]] Result<nlohmann::json> setLeverage(const std::string &contract,
                                                     double leverage);

  public:
    /// Build Gate.io order JSON body from OrderRequest (static, unit-testable).
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
    ///
    /// TradFi CFD format (MT5 style, volume in lots, side 2=buy / 1=sell):
    /// {
    ///   "symbol": "XAUUSD",
    ///   "side": 2,
    ///   "volume": "0.01",
    ///   "price_type": "market" | "trigger",
    ///   "price": "3000"            (trigger only)
    /// }
    [[nodiscard]] static nlohmann::json buildOrderBody(MarketType mt,
                                                       const OrderRequest &req);

    /// Resolve the exchange order id from the TradFi open-orders list
    /// (`data.list` of GET /tradfi/orders). POST /tradfi/orders does NOT echo
    /// the order id — its `data.id` is an internal submission number — so the
    /// caller must match the placed order back against the list.
    ///
    /// The list is newest-first; the FIRST entry matching symbol + side +
    /// volume (+ price for trigger orders) wins. Returns "" when nothing
    /// matches. Pure function (no network) — unit-tested.
    [[nodiscard]] static std::string matchCfdOrderId(const nlohmann::json &list,
                                                     const OrderRequest &req);

  private:
    exchange::GateRestClient &m_restClient;
    MarketType m_marketType;

    /// Cache of the last successfully applied leverage per contract.
    std::mutex m_leverageMutex;
    std::unordered_map<std::string, double> m_leverageCache;

    /// Parse Gate.io order response JSON into OrderResponse.
    ///
    /// Spot:    "id" (string), "status" (open/closed/cancelled)
    /// Futures: "id" (integer), "status" (open/finished), "finish_as" (filled/cancelled)
    /// Cfd:     "id" (number or string), "status" (open/finished/cancelled)
    [[nodiscard]] OrderResponse parseOrderResponse(const nlohmann::json &resp) const;
};

} // namespace pulse::execution
