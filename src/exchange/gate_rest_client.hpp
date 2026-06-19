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
#include "core/error.hpp"
#include "core/types.hpp"

#include <nlohmann/json.hpp>

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
    GateRestClient(GateRestClient &&) noexcept;
    GateRestClient &operator=(GateRestClient &&) noexcept;

    /// Returns true if API key and secret are both non-empty.
    [[nodiscard]] bool has_credentials() const;

    // -----------------------------------------------------------------------
    // Public endpoints (no authentication required)
    // -----------------------------------------------------------------------

    /// GET /api/v4/spot/currencies — list all supported spot currencies.
    ///
    /// This is the "hello world" of Gate.io API — no auth needed, returns
    /// an array of currency metadata (name, precision, deposit/withdraw status).
    [[nodiscard]] Result<nlohmann::json> get_currencies();

    /// GET /api/v4/spot/currency_pairs — list all spot trading pairs.
    ///
    /// Returns pair metadata: base/quote currency, tick size, lot size, min amount.
    [[nodiscard]] Result<nlohmann::json> get_currency_pairs();

    /// GET /api/v4/spot/tickers?currency_pair={pair} — fetch ticker for one pair.
    ///
    /// Returns the latest price, volume, bid/ask for the given trading pair.
    [[nodiscard]] Result<nlohmann::json> get_ticker(const std::string &currency_pair);

    // -----------------------------------------------------------------------
    // Authenticated endpoints (require API key + secret)
    // -----------------------------------------------------------------------

    /// GET /api/v4/spot/accounts — fetch spot account balances.
    ///
    /// Returns an array of {currency, available, locked} objects.
    /// Requires valid API key and secret.
    [[nodiscard]] Result<nlohmann::json> get_spot_accounts();

    // -----------------------------------------------------------------------
    // Futures endpoints
    // -----------------------------------------------------------------------

    /// GET /api/v4/futures/usdt/contracts — list all USDT-settled perpetual contracts.
    ///
    /// Returns contract metadata: name, mark price, funding rate, multiplier, etc.
    [[nodiscard]] Result<nlohmann::json> get_futures_contracts();

    /// GET /api/v4/futures/usdt/tickers?contract={contract} — fetch futures ticker.
    ///
    /// Returns the latest mark price, index price, funding rate, volume.
    [[nodiscard]] Result<nlohmann::json> get_futures_ticker(const std::string &contract);

    /// GET /api/v4/futures/usdt/accounts — fetch futures account balance.
    ///
    /// Returns {total, available, unrealised_pnl, currency, etc.}.
    /// Requires valid API key and secret.
    [[nodiscard]] Result<nlohmann::json> get_futures_accounts();

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
    ExchangeConfig config_;
    MarketType market_type_;

    /// Perform a single HTTP request (no retry). Returns raw HttpResponse.
    [[nodiscard]] HttpResponse do_request(
            const std::string &method,
            const std::string &url,
            const std::string &sign_header_key,
            const std::string &sign_header_sign,
            const std::string &sign_header_timestamp,
            const std::string &body);
};

} // namespace pulse::exchange
