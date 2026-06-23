#pragma once
// gate_auth.hpp — Gate.io v4 API request signing (Layer 1 Exchange)
//
// Gate.io v4 signing algorithm:
//   1. Compute body_hash = SHA-512(request_body) as lowercase hex
//   2. Build sign_payload = method + "\n" + path + "\n" + query + "\n" + body_hash + "\n" + timestamp
//   3. Compute signature = HMAC-SHA512(secret, sign_payload) as lowercase hex
//
// Required headers for authenticated requests:
//   KEY:       <api_key>
//   SIGN:      <signature hex>
//   Timestamp: <unix seconds>
//
// All functions in this header are pure and stateless — safe to call from any thread.

#include <string>

namespace pulse::exchange
{

/// Compute SHA-512 hash of the input data and return as lowercase hex string.
///
/// Used internally to hash the request body as part of the v4 signing payload.
/// An empty body produces the SHA-512 of an empty string (a valid, deterministic value).
[[nodiscard]] std::string sha512Hex(const std::string &data);

/// Compute HMAC-SHA512(secret, payload) and return as lowercase hex string.
///
/// This is the core signing primitive: the secret key is the HMAC key,
/// and the payload is the canonical sign_payload string.
[[nodiscard]] std::string hmacSha512Hex(const std::string &secret, const std::string &payload);

/// Return the current wall-clock time as a Unix-seconds string.
///
/// Gate.io v4 API requires the Timestamp header in seconds (not milliseconds).
[[nodiscard]] std::string unixSeconds();

/// Build the canonical signing payload string for Gate.io v4 API.
///
/// Format: method + "\n" + path + "\n" + query + "\n" + sha512(body) + "\n" + timestamp
///
/// Parameters:
///   1. method    — HTTP method uppercase ("GET", "POST", "DELETE")
///   2. path      — API path including /api/v4 prefix (e.g. "/api/v4/spot/accounts")
///   3. query     — URL query string without leading '?' (empty if none)
///   4. body      — Request body JSON string (empty for GET/DELETE)
///   5. timestamp — Unix seconds string from unixSeconds()
[[nodiscard]] std::string buildSignPayload(
        const std::string &method,
        const std::string &path,
        const std::string &query,
        const std::string &body,
        const std::string &timestamp);

/// Compute the full v4 signature for a request.
///
/// Convenience wrapper: calls buildSignPayload() then hmacSha512Hex().
[[nodiscard]] std::string signRequest(
        const std::string &secret,
        const std::string &method,
        const std::string &path,
        const std::string &query,
        const std::string &body,
        const std::string &timestamp);

} // namespace pulse::exchange
