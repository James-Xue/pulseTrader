#pragma once
// gate_rest_client.hpp — Gate.io v4 REST client (Layer 1 Exchange)
//
// Wraps libcurl to send authenticated HTTP requests to Gate.io's v4 REST API.
//
// Key properties:
//   1. Every request is signed with HMAC-SHA512 using gate_auth functions
//   2. Timeout and retry behaviour are driven by ExchangeConfig
//   3. Responses are returned as nlohmann::json wrapped in Result<T>
//   4. curl_global_init/cleanup are managed via a process-wide RAII guard
//
// Thread safety:
//   - A single GateRestClient instance is NOT thread-safe (libcurl easy handles are per-thread)
//   - Multiple instances can coexist; each creates its own curl easy handle per request
//   - curl_global_init is called exactly once per process via std::call_once

#include "core/config.hpp"
#include "core/PulseError.hpp"
#include "core/types.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// HttpResponse — raw HTTP response before JSON parsing
//
// Exposed for testing and advanced callers that need the status code.
// ---------------------------------------------------------------------------
struct HttpResponse
{
    std::string body;
    long status_code = 0; ///< 0 means the request failed at the transport level.
};

// ---------------------------------------------------------------------------
// GateRestClient — stateless-ish REST client for Gate.io v4 API
//
// "Stateless-ish" because it stores credentials and config, but each request
// creates and destroys its own curl easy handle. No persistent connections.
// ---------------------------------------------------------------------------
class GateRestClient
{
  public:
    /// Construct a REST client from exchange configuration.
    ///
    /// The base URL should be the host only (e.g. "https://api.gateio.ws"),
    /// NOT including "/api/v4" — that prefix is part of the path.
    ///
    /// market_type selects spot or futures endpoint paths for convenience methods.
    /// The generic request() method accepts any path regardless of market_type.
    ///
    /// Initialises curl_global_init() on first construction (process-wide).
    explicit GateRestClient(const ExchangeConfig &config, MarketType market_type = MarketType::Spot);

    ~GateRestClient();

    GateRestClient(const GateRestClient &) = delete;
    GateRestClient &operator=(const GateRestClient &) = delete;
    GateRestClient(GateRestClient &&other) noexcept;
    GateRestClient &operator=(GateRestClient &&other) noexcept;

    /// Returns true if API key and secret are both non-empty.
    [[nodiscard]] bool hasCredentials() const;

    /// Signal all in-flight curl requests to abort early.
    ///
    /// Uses CURLOPT_XFERINFOFUNCTION to cancel curl_easy_perform() at the
    /// next progress callback tick (~1 s).  Thread-safe — called from the
    /// main thread during shutdown while the poll thread is mid-request.
    void cancelRequests();

    // -----------------------------------------------------------------------
    // Public endpoints (no authentication required)
    // -----------------------------------------------------------------------

    /// GET /api/v4/spot/currencies — list all supported spot currencies.
    ///
    /// This is the "hello world" of Gate.io API — no auth needed, returns
    /// an array of currency metadata (name, precision, deposit/withdraw status).
    [[nodiscard]] Result<nlohmann::json> getCurrencies();

    /// GET /api/v4/spot/currency_pairs — list all spot trading pairs.
    ///
    /// Returns pair metadata: base/quote currency, tick size, lot size, min amount.
    [[nodiscard]] Result<nlohmann::json> getCurrencyPairs();

    /// GET /api/v4/spot/tickers?currency_pair={pair} — fetch ticker for one pair.
    ///
    /// Returns the latest price, volume, bid/ask for the given trading pair.
    [[nodiscard]] Result<nlohmann::json> getTicker(const std::string &currency_pair);

    // -----------------------------------------------------------------------
    // Authenticated endpoints (require API key + secret)
    // -----------------------------------------------------------------------

    /// GET /api/v4/spot/accounts — fetch spot account balances.
    ///
    /// Returns an array of {currency, available, locked} objects.
    /// Requires valid API key and secret.
    [[nodiscard]] Result<nlohmann::json> getSpotAccounts();

    // -----------------------------------------------------------------------
    // Futures endpoints
    // -----------------------------------------------------------------------

    /// GET /api/v4/futures/usdt/contracts — list all USDT-settled perpetual contracts.
    ///
    /// Returns contract metadata: name, mark price, funding rate, multiplier, etc.
    [[nodiscard]] Result<nlohmann::json> getFuturesContracts();

    /// GET /api/v4/futures/usdt/tickers?contract={contract} — fetch futures ticker.
    ///
    /// Returns the latest mark price, index price, funding rate, volume.
    [[nodiscard]] Result<nlohmann::json> getFuturesTicker(const std::string &contract);

    /// GET /api/v4/futures/usdt/accounts — fetch futures account balance.
    ///
    /// Returns {total, available, unrealised_pnl, currency, etc.}.
    /// Requires valid API key and secret.
    [[nodiscard]] Result<nlohmann::json> getFuturesAccounts();

    /// GET /api/v4/futures/usdt/positions — list open futures positions.
    ///
    /// Returns an array of position objects, one per contract, with fields:
    /// contract, size (contracts, signed: +long/-short; 0 = no position),
    /// entry_price, mark_price, leverage (0 = cross margin), mode,
    /// liq_price, maintenance_rate, unrealised_pnl, margin, etc.
    ///
    /// Contracts without an open position (size == 0) are filtered out so
    /// the result contains only real exposure. This is the startup
    /// reconciliation source: positions opened by a previous engine run (or
    /// manually) become visible to the risk engine after a restart.
    [[nodiscard]] Result<nlohmann::json> getFuturesPositions();

    /// GET /api/v4/futures/usdt/accounts — fetch and parse futures account balance.
    ///
    /// Convenience wrapper around getFuturesAccounts() that returns a parsed
    /// AccountBalance struct with total equity, available balance, unrealized PnL, etc.
    [[nodiscard]] Result<AccountBalance> getFuturesAccountBalance();

    /// POST /api/v4/futures/usdt/orders — place a futures order.
    ///
    /// body should contain: contract, size, price, tif, text, reduce_only, etc.
    /// Returns the order object on success.
    [[nodiscard]] Result<nlohmann::json> postFuturesOrder(const nlohmann::json &body);

    /// DELETE /api/v4/futures/usdt/orders/{order_id} — cancel a futures order.
    ///
    /// Returns the cancelled order object on success.
    [[nodiscard]] Result<nlohmann::json> cancelFuturesOrder(const std::string &order_id);

    /// GET /api/v4/futures/usdt/orders/{order_id} — query a futures order.
    ///
    /// Returns the order object with current status.
    [[nodiscard]] Result<nlohmann::json> getFuturesOrder(const std::string &order_id);

    /// GET /api/v4/futures/usdt/orders?contract=X&status=open — list ALL open
    /// futures orders for a contract from the exchange, including orders the
    /// engine did not place itself (App-side orders, pre-restart leftovers).
    /// The engine's tracker-based get_orders view misses those — M23 grid
    /// management needs the exchange truth.
    [[nodiscard]] Result<nlohmann::json> getFuturesOrders(const std::string &contract);

    // -----------------------------------------------------------------------
    // Futures trigger orders (price_orders) — the TP/SL attached to a futures
    // position. They live on a SEPARATE endpoint from plain orders; the
    // position's close_order field does NOT show them (2026-08-16 memo).
    // -----------------------------------------------------------------------

    /// POST /api/v4/futures/usdt/price_orders — create a futures trigger order.
    ///
    /// body: {"initial": {contract, size, price, tif, is_reduce_only, ...},
    ///        "trigger": {price, rule (1=above / 2=below), price_type,
    ///                    expiration}, "order_type": "close-short-position"}
    /// Returns the trigger order object on success.
    [[nodiscard]] Result<nlohmann::json> postFuturesPriceOrder(const nlohmann::json &body);

    /// GET /api/v4/futures/usdt/price_orders?contract=X&status=open — list
    /// open trigger orders for a contract (status=open filter hard-coded).
    [[nodiscard]] Result<nlohmann::json> getFuturesPriceOrders(const std::string &contract);

    /// DELETE /api/v4/futures/usdt/price_orders/{order_id} — cancel a trigger
    /// order. Returns the cancelled trigger order object on success.
    [[nodiscard]] Result<nlohmann::json> cancelFuturesPriceOrder(const std::string &order_id);

    // -----------------------------------------------------------------------
    // TradFi (CFD) endpoints — /api/v4/tradfi/*, MT5 account, USD settlement
    // -----------------------------------------------------------------------

    /// GET /api/v4/tradfi/symbols — list all CFD symbols (public).
    [[nodiscard]] Result<nlohmann::json> getCfdSymbols();

    /// GET /api/v4/tradfi/symbols/{symbol}/tickers — per-symbol CFD ticker (public).
    [[nodiscard]] Result<nlohmann::json> getCfdTicker(const std::string &symbol);

    /// GET /api/v4/tradfi/symbols/{symbol}/klines — per-symbol CFD klines.
    ///
    /// Query: kline_type=1m&limit=N (max 500). Times are Unix seconds.
    [[nodiscard]] Result<nlohmann::json> getCfdKlines(const std::string &symbol, int limit = 500);

    /// GET /api/v4/tradfi/symbols/detail?symbols={sym} — contract specs.
    ///
    /// Requires auth. Returns contract_volume, min/max/step_order_volume,
    /// leverage/leverages[], price_precision, settlement_currency.
    [[nodiscard]] Result<nlohmann::json> getCfdSymbolsDetail(const std::vector<std::string> &symbols);

    /// GET /api/v4/tradfi/users/assets — CFD account balance (USD).
    ///
    /// Returns equity, balance, margin, margin_free, unrealized_pnl, storage, outable.
    [[nodiscard]] Result<nlohmann::json> getCfdAssets();

    /// POST /api/v4/tradfi/transactions — transfer funds between the main
    /// account and the CFD (MT5) account.
    ///
    /// asset: "USDT" only. change: amount (≤2 decimals). type: "deposit"
    /// (main → CFD) or "withdraw" (CFD → main).
    [[nodiscard]] Result<nlohmann::json> postCfdTransfer(const std::string &asset,
                                                         const std::string &change,
                                                         const std::string &type);

    /// GET /api/v4/tradfi/positions — active CFD positions.
    [[nodiscard]] Result<nlohmann::json> getCfdPositions();

    /// POST /api/v4/tradfi/positions/{position_id}/close — close a CFD position.
    ///
    /// close_type: 1 = partial (close_volume required), 2 = full.
    [[nodiscard]] Result<nlohmann::json> postCfdPositionClose(const std::string &position_id,
                                                              int close_type,
                                                              double close_volume = 0.0);

    /// POST /api/v4/tradfi/orders — place a CFD order (MT5-style body).
    [[nodiscard]] Result<nlohmann::json> postCfdOrder(const nlohmann::json &body);

    /// GET /api/v4/tradfi/orders/{order_id} — query a CFD order.
    [[nodiscard]] Result<nlohmann::json> getCfdOrder(const std::string &order_id);

    /// GET /api/v4/tradfi/orders — the open-orders list. Used for CFD order
    /// id resolution and status polling (the single-order GET does not exist).
    [[nodiscard]] Result<nlohmann::json> getCfdOrders();

    /// DELETE /api/v4/tradfi/orders/{order_id} — cancel a CFD order.
    [[nodiscard]] Result<nlohmann::json> cancelCfdOrder(const std::string &order_id);

    /// Dynamically adjust the protective stops on an open CFD position —
    /// PUT /tradfi/positions/{id} with price_sl / price_tp ("0" clears).
    [[nodiscard]] Result<nlohmann::json> putCfdPositionModify(
        const std::string &position_id, const std::string &sl_price,
        const std::string &tp_price);

    /// Set futures position leverage for a contract (dual/hedge mode).
    ///
    /// Gate.io applies leverage at the position level, NOT per order — a
    /// new order silently inherits the account's current setting unless
    /// this is called first. Cross margin semantics (engine default):
    /// leverage="0" selects cross margin, cross_leverage_limit carries the
    /// multiple. Uses the dual_comp (hedge mode) endpoint.
    ///
    /// Parameters:
    ///   1. contract — futures contract (e.g. "BTC_USDT")
    ///   2. leverage — leverage multiple (e.g. 10 for 10x cross margin)
    [[nodiscard]] Result<nlohmann::json> setFuturesLeverage(const std::string &contract,
                                                            double leverage);

    // -----------------------------------------------------------------------
    // Generic request (for future expansion)
    // -----------------------------------------------------------------------

    /// Send an arbitrary signed request to Gate.io v4 API.
    ///
    /// Parameters:
    ///   1. method — HTTP method ("GET", "POST", "DELETE")
    ///   2. path   — full API path including /api/v4 (e.g. "/api/v4/spot/orders")
    ///   3. query  — URL query string without leading '?' (empty if none)
    ///   4. body   — request body (empty for GET/DELETE)
    ///
    /// Handles signing, timeout, retries, and JSON parsing.
    /// Returns the parsed JSON body on success, or PulseError on failure.
    [[nodiscard]] Result<nlohmann::json> request(
            const std::string &method,
            const std::string &path,
            const std::string &query = "",
            const std::string &body = "");

  private:
    ExchangeConfig m_config;
    MarketType m_marketType;

    /// When set to true, in-flight curl requests abort at the next progress tick.
    std::atomic<bool> m_abortRequested{ false };

    /// Perform a single HTTP request (no retry). Returns raw HttpResponse.
    [[nodiscard]] HttpResponse doRequest(
            const std::string &method,
            const std::string &url,
            const std::string &sign_header_key,
            const std::string &sign_header_sign,
            const std::string &sign_header_timestamp,
            const std::string &body);
};

} // namespace pulse::exchange
