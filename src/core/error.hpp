#pragma once
// error.hpp — Error codes and Result<T> for pulseTrader
//
// Design rationale:
//   1. Exceptions are too slow for the hot path (L1→L3→L6→L7→L8)
//   2. Result<T> is a lightweight std::variant<T, PulseError> — zero overhead
//   3. ErrorCode enum groups errors by subsystem (1xxx network, 2xxx exchange, etc.)
//   4. Callers check ok(r) / value(r) / error(r) — no try/catch needed

#include <cstdint>
#include <string>
#include <variant>

namespace pulse
{

// ---------------------------------------------------------------------------
// ErrorCode — categorised by subsystem range
//
//   0      : success
//   1xxx   : network / connectivity
//   2xxx   : exchange-specific (rate limits, balances, order validation)
//   3xxx   : risk management rejections
//   4xxx   : AI backend failures
//   5xxx   : config loader
//   6xxx   : trade recorder (SQLite)
//   9xxx   : internal / programming errors
//   91xx   : WebUI server errors
// ---------------------------------------------------------------------------
enum class ErrorCode : std::uint32_t
{
    Ok = 0,

    // Network (1xxx)
    NetworkTimeout = 1000,
    NetworkDisconnected = 1001,
    HttpError = 1002,

    // WebSocket (1xxx)
    WsConnectionFailed = 1010, ///< Initial connection or reconnect failed.
    WsParseError = 1011,       ///< Incoming frame is not valid JSON.
    WsAuthFailed = 1012,       ///< Private channel authentication rejected.
    WsReconnectLimit = 1013,   ///< Reconnect attempts exceeded maximum.

    // Exchange (2xxx)
    RateLimitExceeded = 2000,
    InsufficientBalance = 2001,
    InvalidOrder = 2002,
    ExchangeError = 2003,

    // Risk (3xxx)
    OrderRejected = 3000,
    DrawdownLimitHit = 3001,
    PositionLimitHit = 3002,
    RateLimitHit = 3003,       ///< Order rate limiter rejected (token bucket empty).
    StopLossTriggered = 3004,  ///< Position closed by stop-loss engine.
    TakeProfitTriggered = 3005, ///< Position partially/fully closed by take-profit engine.
    SymbolLimitHit = 3006,     ///< Per-symbol notional limit exceeded.

    // AI (4xxx)
    AiResponseInvalid = 4000,  ///< LLM response could not be parsed or validated.
    AiBackendError = 4001,     ///< LLM provider returned a non-retryable error.
    AiTimeout = 4002,          ///< LLM request exceeded configured timeout.
    AiSchemaMismatch = 4003,   ///< LLM response does not match the required JSON schema.
    AiParamOutOfBounds = 4004, ///< AI-proposed delta exceeded safety bounds (clamped, not fatal).

    // Config (5xxx)
    ConfigFileNotFound = 5000,  ///< TOML file path does not exist.
    ConfigParseError = 5001,    ///< toml11 syntax error (malformed TOML).
    ConfigMissingField = 5002,  ///< Required key absent from TOML tree.
    ConfigInvalidValue = 5003,  ///< Value present but outside valid range.
    ConfigEnvVarMissing = 5004, ///< from_env:VAR referenced but VAR is unset/empty.
    ConfigValidationError = 5005, ///< Semantic validation failure (cross-field).

    // Trade Recorder (6xxx)
    TradeRecorderDbError = 6000,      ///< SQLite open/create failed.
    TradeRecorderInsertFailed = 6001, ///< INSERT statement execution failed.
    TradeRecorderQueryFailed = 6002,  ///< SELECT query execution failed.
    TradeRecorderNotOpen = 6003,      ///< Database connection is not open.
    TradeRecorderSchemaError = 6004,  ///< Table creation or migration failed.
    TradeRecorderDuplicate = 6005,    ///< Duplicate order_id (UNIQUE constraint).

    // WebUI (91xx)
    WebUiBindFailed = 9100,     ///< Failed to bind to listen address/port.
    WebUiAuthFailed = 9101,     ///< Bearer token authentication rejected.
    WebUiHostInvalid = 9102,    ///< Host header validation failed (DNS rebinding attempt).
    WebUiClientLimit = 9103,    ///< WebSocket client limit reached.
    WebUiSnapshotError = 9104,  ///< Failed to assemble dashboard snapshot.
    WebUiStaticNotFound = 9105, ///< Requested static file not found.

    // Internal (9xxx)
    InternalError = 9000,
    NotImplemented = 9001,
};

// ---------------------------------------------------------------------------
// PulseError — error value carrying a code + human-readable message
// ---------------------------------------------------------------------------
struct PulseError
{
    ErrorCode code;
    std::string message;
};

// ---------------------------------------------------------------------------
// Result<T> — success-or-error wrapper for hot-path functions
//
// Usage:
//   Result<double> get_price() { return 65000.0; }        // ok
//   Result<double> get_price() {                          // error
//       return PulseError{ErrorCode::NetworkTimeout, "..."}; }
//
//   auto r = get_price();
//   if (ok(r)) { use(value(r)); }
//   else       { log(error(r).message); }
// ---------------------------------------------------------------------------
template <typename T> using Result = std::variant<T, PulseError>;

/// Returns true if the Result holds a success value (T).
template <typename T> [[nodiscard]] bool ok(const Result<T> &r) noexcept
{
    return std::holds_alternative<T>(r);
}

/// Returns a const reference to the success value. UB if !ok(r).
template <typename T> [[nodiscard]] const T &value(const Result<T> &r)
{
    return std::get<T>(r);
}

/// Returns a mutable reference to the success value. UB if !ok(r).
template <typename T> [[nodiscard]] T &value(Result<T> &r)
{
    return std::get<T>(r);
}

/// Returns a const reference to the error. UB if ok(r).
template <typename T> [[nodiscard]] const PulseError &error(const Result<T> &r)
{
    return std::get<PulseError>(r);
}

} // namespace pulse
