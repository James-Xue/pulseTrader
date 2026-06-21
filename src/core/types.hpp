#pragma once
// types.hpp — Fundamental type aliases and enumerations for pulseTrader
//
// Centralises domain types so every layer speaks the same vocabulary:
//   1. Timestamp   — nanosecond-precision wall-clock time
//   2. Symbol      — Gate.io trading pair (e.g. "BTC_USDT")
//   3. Price       — quote currency amount (e.g. USDT)
//   4. Quantity    — base currency amount (e.g. BTC)
//   5. Side        — Buy / Sell
//   6. OrderType   — Market / Limit / PostOnly
//   7. OrderStatus — lifecycle state of an order
//   8. MarketType  — Spot / Futures discriminator
//   9. MarginMode  — Cross / Isolated (futures only)

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

/// Market type discriminator: Spot or Futures (USDT-settled perpetual).
enum class MarketType : std::uint8_t
{
    Spot,    ///< Spot trading — currency pairs like BTC_USDT.
    Futures, ///< Perpetual futures — contracts like BTC_USDT (USDT-settled).
};

/// Margin mode for futures positions.
enum class MarginMode : std::uint8_t
{
    Cross,    ///< Cross margin — shared across all positions.
    Isolated, ///< Isolated margin — dedicated margin per position.
};

/// Exchange-reported account balance (futures).
///
/// Fetched via GET /api/v4/futures/{settle}/accounts.
/// All monetary values are in the settlement currency (e.g., USDT).
struct AccountBalance
{
    double total           = 0.0; ///< Total equity (available + margins + unrealised PnL).
    double available       = 0.0; ///< Available for new orders.
    double unrealised_pnl  = 0.0; ///< Unrealized PnL from open positions.
    double position_margin = 0.0; ///< Margin locked in positions.
    double order_margin    = 0.0; ///< Margin reserved for open orders.
    std::string currency;         ///< Settlement currency (e.g. "USDT").
};

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

/// Returns the opposite trade direction (Buy ↔ Sell).
[[nodiscard]] constexpr Side opposite(Side s) noexcept
{
    return Side::Buy == s ? Side::Sell : Side::Buy;
}

/// Convert MarketType to a human-readable string.
[[nodiscard]] constexpr const char* to_string(MarketType mt) noexcept
{
    switch (mt)
    {
    case MarketType::Spot:
        return "spot";
    case MarketType::Futures:
        return "futures";
    }
    return "unknown";
}

/// Convert MarginMode to a human-readable string.
[[nodiscard]] constexpr const char* to_string(MarginMode mm) noexcept
{
    switch (mm)
    {
    case MarginMode::Cross:
        return "cross";
    case MarginMode::Isolated:
        return "isolated";
    }
    return "unknown";
}

/// Parse a string to double without throwing exceptions.
///
/// Uses std::from_chars (C++17) for locale-independent, non-throwing parsing.
/// Returns std::nullopt on empty input, whitespace-only input, or parse failure.
///
/// This is the safe replacement for std::stod() in exchange data parsing —
/// std::stod throws std::invalid_argument on malformed input, which can crash
/// the WebSocket event thread.
[[nodiscard]] inline std::optional<double> safe_parse_double(std::string_view sv) noexcept
{
    if (sv.empty())
    {
        return std::nullopt;
    }
    double result = 0.0;
    const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (std::make_error_code(ec) != std::errc{} || ptr != sv.data() + sv.size())
    {
        return std::nullopt;
    }
    return result;
}

} // namespace pulse
