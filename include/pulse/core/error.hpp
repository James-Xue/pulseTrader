#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace pulse {

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

enum class ErrorCode : std::uint32_t {
    Ok = 0,

    // Network (1xxx)
    NetworkTimeout      = 1000,
    NetworkDisconnected = 1001,
    HttpError           = 1002,

    // Exchange (2xxx)
    RateLimitExceeded   = 2000,
    InsufficientBalance = 2001,
    InvalidOrder        = 2002,
    ExchangeError       = 2003,

    // Risk (3xxx)
    OrderRejected       = 3000,
    DrawdownLimitHit    = 3001,
    PositionLimitHit    = 3002,

    // AI (4xxx)
    AiResponseInvalid   = 4000,
    AiBackendError      = 4001,

    // Internal (9xxx)
    InternalError       = 9000,
    NotImplemented      = 9001,
};

// ---------------------------------------------------------------------------
// Error value
// ---------------------------------------------------------------------------

struct PulseError {
    ErrorCode   code;
    std::string message;
};

// ---------------------------------------------------------------------------
// Result<T> — avoids exceptions on hot paths
//
// Usage:
//   Result<double> get_price() { return 65000.0; }        // ok
//   Result<double> get_price() {                          // error
//       return PulseError{ErrorCode::NetworkTimeout, "..."}; }
//
//   auto r = get_price();
//   if (ok(r)) { use(value(r)); }
//   else        { log(error(r).message); }
// ---------------------------------------------------------------------------

template <typename T>
using Result = std::variant<T, PulseError>;

template <typename T>
[[nodiscard]] bool ok(const Result<T>& r) noexcept {
    return std::holds_alternative<T>(r);
}

template <typename T>
[[nodiscard]] const T& value(const Result<T>& r) {
    return std::get<T>(r);
}

template <typename T>
[[nodiscard]] T& value(Result<T>& r) {
    return std::get<T>(r);
}

template <typename T>
[[nodiscard]] const PulseError& error(const Result<T>& r) {
    return std::get<PulseError>(r);
}

} // namespace pulse
