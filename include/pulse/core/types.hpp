#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace pulse {

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/// Nanosecond-precision wall-clock timestamp.
using Timestamp = std::chrono::time_point<
    std::chrono::system_clock,
    std::chrono::nanoseconds>;

using Duration = std::chrono::nanoseconds;

/// Returns the current wall-clock time with nanosecond precision.
[[nodiscard]] inline Timestamp now() noexcept {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());
}

// ---------------------------------------------------------------------------
// Market identifiers
// ---------------------------------------------------------------------------

/// Trading pair symbol in Gate.io format, e.g. "BTC_USDT".
using Symbol = std::string;

/// Price in quote currency (e.g. USDT).
using Price = double;

/// Order/position quantity in base currency (e.g. BTC).
using Quantity = double;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// Trade direction.
enum class Side : std::uint8_t {
    Buy,
    Sell,
};

/// Order types supported by Gate.io.
enum class OrderType : std::uint8_t {
    Market,
    Limit,
    PostOnly,   ///< Rejected if it would match immediately (maker-only).
};

/// Lifecycle state of an order.
enum class OrderStatus : std::uint8_t {
    Pending,
    Open,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
};

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/// Returns the opposite side.
[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

} // namespace pulse
