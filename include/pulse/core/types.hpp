#pragma once
// types.hpp — Fundamental type aliases and enumerations for pulseTrader
//
// Centralises domain types so every layer speaks the same vocabulary:
//   1. Timestamp  — nanosecond-precision wall-clock time
//   2. Symbol     — Gate.io trading pair (e.g. "BTC_USDT")
//   3. Price      — quote currency amount (e.g. USDT)
//   4. Quantity   — base currency amount (e.g. BTC)
//   5. Side       — Buy / Sell
//   6. OrderType  — Market / Limit / PostOnly
//   7. OrderStatus — lifecycle state of an order

#include <chrono>
#include <cstdint>
#include <string>

namespace pulse
{

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/// Nanosecond-precision wall-clock timestamp (system_clock-based).
/// Using system_clock (not steady_clock) because exchange APIs use wall time.
using Timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

/// Duration type matching Timestamp's precision.
using Duration = std::chrono::nanoseconds;

/// Returns the current wall-clock time with nanosecond precision.
[[nodiscard]] inline Timestamp now() noexcept
{
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}

// ---------------------------------------------------------------------------
// Market identifiers
// ---------------------------------------------------------------------------

/// Trading pair symbol in Gate.io format, e.g. "BTC_USDT".
using Symbol = std::string;

/// Price in quote currency (e.g. USDT). Stored as double for simplicity;
/// fixed-point can be introduced later if precision becomes a concern.
using Price = double;

/// Order/position quantity in base currency (e.g. BTC).
using Quantity = double;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// Trade direction: Buy or Sell.
enum class Side : std::uint8_t
{
    Buy,
    Sell,
};

/// Order types supported by Gate.io spot/futures API.
enum class OrderType : std::uint8_t
{
    Market,   ///< Execute immediately at best available price.
    Limit,    ///< Place at a specific price; may rest in the book.
    PostOnly, ///< Rejected if it would match immediately (maker-only).
};

/// Lifecycle state of an order from submission to terminal state.
///
/// State machine:
///   Pending → Open → PartiallyFilled → Filled
///                  → Cancelled
///         → Rejected
enum class OrderStatus : std::uint8_t
{
    Pending,         ///< Submitted but not yet acknowledged by the exchange.
    Open,            ///< Acknowledged and resting in the order book.
    PartiallyFilled, ///< Part of the order has been filled.
    Filled,          ///< Fully filled — terminal state.
    Cancelled,       ///< Cancelled by user or system — terminal state.
    Rejected,        ///< Rejected by exchange or risk engine — terminal state.
};

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/// Returns the opposite trade direction (Buy ↔ Sell).
[[nodiscard]] constexpr Side opposite(Side s) noexcept
{
    return Side::Buy == s ? Side::Sell : Side::Buy;
}

} // namespace pulse
