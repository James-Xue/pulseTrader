// gate_rest_client.cpp — Gate.io v4 REST client implementation (Layer 1 Exchange)
//
// Architecture:
//   1. Each public method delegates to request() which handles the full lifecycle:
//      build URL → sign → send HTTP → retry on transient failure → parse JSON
//   2. curl_global_init is called exactly once via std::call_once (process-safe)
//   3. Each request creates its own curl easy handle — no persistent connections
//   4. Retries use exponential backoff for transient HTTP errors (5xx, timeout)
//
// Error classification:
//   - HTTP 429       → ErrorCode::RateLimitExceeded
//   - HTTP 4xx       → ErrorCode::HttpError (client error, no retry)
//   - HTTP 5xx       → ErrorCode::HttpError (server error, retry eligible)
//   - curl failure   → ErrorCode::NetworkDisconnected
//   - JSON parse fail → ErrorCode::ExchangeError
//   - HTTP timeout   → ErrorCode::NetworkTimeout

#include "exchange/gate_rest_client.hpp"

#include "exchange/endpoint_router.hpp"
#include "exchange/gate_auth.hpp"
#include "logging/logger.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// Process-wide curl initialisation (RAII guard)
//
// curl_global_init must be called once per process before any curl_easy_init.
// curl_global_cleanup must be called once after the last curl operation.
// std::call_once guarantees thread-safe one-time initialisation.
// ---------------------------------------------------------------------------

namespace
{

std::once_flag g_curl_init_flag;

void ensure_curl_init()
{
    std::call_once(g_curl_init_flag, []()
            {
                curl_global_init(CURL_GLOBAL_DEFAULT);
                // Note: curl_global_cleanup is intentionally NOT registered with atexit().
                // In practice, the OS reclaims resources on process exit. If clean shutdown
                // is needed, call curl_global_cleanup() explicitly after all clients are destroyed.
            });
}

// libcurl write callback: appends received data chunks to a string buffer.
// 1. Cast the user-data pointer to our response body string
// 2. Append the received chunk
// 3. Return the number of bytes consumed (must equal size * nmemb or curl aborts)
size_t curl_write_callback(void *contents, size_t size, size_t nmemb, std::string *output)
{
    const size_t total = size * nmemb;
    output->append(static_cast<char *>(contents), total);
    return total;
}

// Map HTTP status code to an ErrorCode.
// 1. 429 → RateLimitExceeded (exchange is throttling us)
// 2. 4xx → HttpError (client-side issue, retrying won't help)
// 3. 5xx → HttpError (server-side issue, retry may help — caller decides)
ErrorCode classify_http_error(long status_code)
{
    if (429 == status_code)
    {
        return ErrorCode::RateLimitExceeded;
    }
    return ErrorCode::HttpError;
}

// Determine if an HTTP error is retryable (only 5xx server errors).
bool is_retryable(long status_code)
{
    return status_code >= 500 && status_code < 600;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// GateRestClient implementation
// ---------------------------------------------------------------------------

GateRestClient::GateRestClient(const ExchangeConfig &config, MarketType market_type)
    : config_(config), market_type_(market_type)
{
    ensure_curl_init();
}

GateRestClient::~GateRestClient() = default;
GateRestClient::GateRestClient(GateRestClient &&) noexcept = default;
GateRestClient &GateRestClient::operator=(GateRestClient &&) noexcept = default;

bool GateRestClient::has_credentials() const
{
    return !config_.apiKey.empty() && !config_.apiSecret.empty();
}

// ---------------------------------------------------------------------------
// do_request — single HTTP request, no retry
//
// Steps:
//   1. Create a curl easy handle
//   2. Set URL, headers (Content-Type + auth), write callback
//   3. Configure method-specific options (POST body, DELETE custom request)
//   4. Set timeout from config
//   5. Execute and collect response
//   6. Clean up handle and return
// ---------------------------------------------------------------------------
HttpResponse GateRestClient::do_request(
        const std::string &method,
        const std::string &url,
        const std::string &sign_header_key,
        const std::string &sign_header_sign,
        const std::string &sign_header_timestamp,
        const std::string &body)
{
    HttpResponse response;

    CURL *curl = curl_easy_init();
    if (nullptr == curl)
    {
        response.body = "curl_easy_init failed";
        return response;
    }

    // 1. Build HTTP headers
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // 2. Add auth headers only when credentials are available
    if (!sign_header_key.empty())
    {
        headers = curl_slist_append(headers, ("KEY: " + sign_header_key).c_str());
        headers = curl_slist_append(headers, ("SIGN: " + sign_header_sign).c_str());
        headers = curl_slist_append(headers, ("Timestamp: " + sign_header_timestamp).c_str());
    }

    // 3. Configure the request
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config_.restTimeoutMs));

    // Proxy support — read from environment (HTTPS_PROXY / HTTP_PROXY)
    if (!config_.proxyUrl.empty())
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, config_.proxyUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
    }
    else if (const char *proxy = std::getenv("HTTPS_PROXY"); proxy)
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
        curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
    }
    else if (const char *http_proxy = std::getenv("HTTP_PROXY"); http_proxy)
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, http_proxy);
        curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L);
    }

    // 4. Method-specific configuration
    if ("POST" == method)
    {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    else if ("GET" != method)
    {
        // DELETE or other custom methods
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty())
        {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }
    // GET is the default — no extra configuration needed

    // 5. Execute
    CURLcode res = curl_easy_perform(curl);
    if (CURLE_OK == res)
    {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    }
    else
    {
        // Transport-level failure (DNS, TCP, TLS, timeout, etc.)
        response.status_code = 0;
        response.body = "curl error " + std::to_string(static_cast<int>(res)) + ": " + curl_easy_strerror(res);
    }

    // 6. Clean up
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return response;
}

// ---------------------------------------------------------------------------
// request — full lifecycle: sign → send → retry → parse JSON
//
// Steps:
//   1. Generate timestamp and compute HMAC-SHA512 signature
//   2. Build full URL from base + path + query
//   3. Attempt the request up to maxRetries+1 times
//   4. On transient failure (5xx / transport error), sleep and retry
//   5. On success, parse the response body as JSON
//   6. Return Result<json> — either the parsed JSON or a PulseError
// ---------------------------------------------------------------------------
Result<nlohmann::json> GateRestClient::request(
        const std::string &method,
        const std::string &path,
        const std::string &query,
        const std::string &body)
{
    // 1. Sign the request
    const std::string timestamp = unix_seconds();
    std::string sign;
    if (has_credentials())
    {
        sign = sign_request(config_.apiSecret, method, path, query, body, timestamp);
    }

    // 2. Build full URL
    std::string url = config_.restBaseUrl + path;
    if (!query.empty())
    {
        url += "?" + query;
    }

    PULSE_LOG_DEBUG("exchange", "{} {} (timeout={}ms, retries={})", method, url, config_.restTimeoutMs, config_.maxRetries);

    // 3. Retry loop
    const std::uint32_t max_attempts = config_.maxRetries + 1;
    HttpResponse response;

    for (std::uint32_t attempt = 1; attempt <= max_attempts; ++attempt)
    {
        response = do_request(method, url, config_.apiKey, sign, timestamp, body);

        // Success — break out of retry loop
        if (response.status_code >= 200 && response.status_code < 300)
        {
            break;
        }

        // Retryable failure — exponential backoff (100ms, 200ms, 400ms, ...)
        if (0 == response.status_code || is_retryable(response.status_code))
        {
            if (attempt < max_attempts)
            {
                const auto backoff_ms = std::chrono::milliseconds(100 * (1u << (attempt - 1)));
                PULSE_LOG_WARN("exchange",
                        "attempt {}/{} failed (status={}), retrying in {}ms",
                        attempt,
                        max_attempts,
                        response.status_code,
                        backoff_ms.count());
                std::this_thread::sleep_for(backoff_ms);
                continue;
            }
        }

        // Non-retryable failure (4xx except 429 handled separately) — stop immediately
        break;
    }

    // 4. Check transport-level failure (curl error, no HTTP status)
    if (0 == response.status_code)
    {
        PULSE_LOG_ERROR("exchange", "request failed: {}", response.body);
        return PulseError{ErrorCode::NetworkDisconnected, response.body};
    }

    // 5. Check HTTP error status
    if (response.status_code < 200 || response.status_code >= 300)
    {
        const ErrorCode code = classify_http_error(response.status_code);
        std::string msg = "HTTP " + std::to_string(response.status_code) + ": " + response.body;
        PULSE_LOG_WARN("exchange", "{} {} → {}", method, path, msg);
        return PulseError{code, std::move(msg)};
    }

    // 6. Parse JSON response
    auto json_data = nlohmann::json::parse(response.body, nullptr, false);
    if (json_data.is_discarded())
    {
        PULSE_LOG_ERROR("exchange", "JSON parse failed for {} {}: {}", method, path, response.body.substr(0, 200));
        return PulseError{ErrorCode::ExchangeError, "JSON parse failed"};
    }

    return json_data;
}

// ---------------------------------------------------------------------------
// Public endpoint wrappers
// ---------------------------------------------------------------------------

Result<nlohmann::json> GateRestClient::get_currencies()
{
    return request("GET", "/api/v4/spot/currencies");
}

Result<nlohmann::json> GateRestClient::get_currency_pairs()
{
    return request("GET", "/api/v4/spot/currency_pairs");
}

Result<nlohmann::json> GateRestClient::get_ticker(const std::string &currency_pair)
{
    return request("GET", "/api/v4/spot/tickers", "currency_pair=" + currency_pair);
}

Result<nlohmann::json> GateRestClient::get_spot_accounts()
{
    if (!has_credentials())
    {
        return PulseError{ErrorCode::HttpError, "Missing API key/secret — cannot access authenticated endpoint"};
    }
    return request("GET", "/api/v4/spot/accounts");
}

// ---------------------------------------------------------------------------
// Futures endpoint wrappers
// ---------------------------------------------------------------------------

Result<nlohmann::json> GateRestClient::get_futures_contracts()
{
    return request("GET", EndpointRouter::contracts_path(MarketType::Futures));
}

Result<nlohmann::json> GateRestClient::get_futures_ticker(const std::string &contract)
{
    return request("GET", EndpointRouter::tickers_path(MarketType::Futures), "contract=" + contract);
}

Result<nlohmann::json> GateRestClient::get_futures_accounts()
{
    if (!has_credentials())
    {
        return PulseError{ErrorCode::HttpError, "Missing API key/secret — cannot access authenticated endpoint"};
    }
    return request("GET", EndpointRouter::accounts_path(MarketType::Futures));
}

Result<AccountBalance> GateRestClient::get_futures_account_balance()
{
    auto result = get_futures_accounts();
    if (!ok(result))
    {
        return error(result);
    }

    const auto &j = value(result);
    AccountBalance bal;
    bal.total           = safe_parse_double(j.value("total", "0")).value_or(0.0);
    bal.available       = safe_parse_double(j.value("available", "0")).value_or(0.0);
    bal.unrealised_pnl  = safe_parse_double(j.value("unrealised_pnl", "0")).value_or(0.0);
    bal.position_margin = safe_parse_double(j.value("position_margin", "0")).value_or(0.0);
    bal.order_margin    = safe_parse_double(j.value("order_margin", "0")).value_or(0.0);
    bal.currency        = j.value("currency", "USDT");
    return bal;
}

Result<nlohmann::json> GateRestClient::post_futures_order(const nlohmann::json &body)
{
    if (!has_credentials())
    {
        return PulseError{ErrorCode::HttpError, "Missing API key/secret — cannot place futures order"};
    }
    return request("POST", EndpointRouter::orders_path(MarketType::Futures), "", body.dump());
}

Result<nlohmann::json> GateRestClient::cancel_futures_order(const std::string &order_id)
{
    if (!has_credentials())
    {
        return PulseError{ErrorCode::HttpError, "Missing API key/secret — cannot cancel futures order"};
    }
    return request("DELETE", EndpointRouter::order_path(MarketType::Futures, order_id));
}

Result<nlohmann::json> GateRestClient::get_futures_order(const std::string &order_id)
{
    if (!has_credentials())
    {
        return PulseError{ErrorCode::HttpError, "Missing API key/secret — cannot query futures order"};
    }
    return request("GET", EndpointRouter::order_path(MarketType::Futures, order_id));
}

} // namespace pulse::exchange
