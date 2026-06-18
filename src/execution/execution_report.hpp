#pragma once
// execution_report.hpp — Immutable fill record (Layer 8 Order Execution)
//
// Generated when an order reaches terminal state (Filled or Cancelled).
// Contains all relevant execution metrics: fill price, slippage, fees, latency.

#include "core/types.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>

namespace pulse::execution
{

// ---------------------------------------------------------------------------
// ExecutionReport — immutable record of a completed order lifecycle
// ---------------------------------------------------------------------------
struct ExecutionReport
{
    std::string order_id;           ///< Exchange-assigned order ID.
    std::string client_order_id;    ///< Client-assigned order ID (may be empty).
    Symbol symbol;                  ///< Trading pair (e.g. "BTC_USDT").
    Side side;                      ///< Buy or Sell.
    OrderType type;                 ///< Market, Limit, or PostOnly.
    Quantity requested_qty;         ///< Original order quantity.
    Quantity filled_qty;            ///< Actually filled quantity.
    Price avg_fill_price;           ///< Volume-weighted average fill price.
    Price submit_mid_price;         ///< Mid-price at submission time (for slippage calc).
    Price slippage_bps;             ///< Slippage in basis points: (fill_price - mid_price) / mid_price * 10000.
    Price fees;                     ///< Total fees paid (in quote currency).
    std::chrono::milliseconds latency; ///< Latency from submission to fill.
    Timestamp submit_time;          ///< When the order was submitted.
    Timestamp fill_time;            ///< When the order was filled (or cancelled).
    OrderStatus final_status;       ///< Filled or Cancelled (terminal state).

    /// Default constructor — zero-initializes all fields.
    ExecutionReport()
        : order_id{}
        , client_order_id{}
        , symbol{}
        , side{ Side::Buy }
        , type{ OrderType::Limit }
        , requested_qty{ 0.0 }
        , filled_qty{ 0.0 }
        , avg_fill_price{ 0.0 }
        , submit_mid_price{ 0.0 }
        , slippage_bps{ 0.0 }
        , fees{ 0.0 }
        , latency{ 0 }
        , submit_time{}
        , fill_time{}
        , final_status{ OrderStatus::Pending }
    {
    }

    /// Serialize to JSON for logging/persistence.
    [[nodiscard]] nlohmann::json to_json() const;

    /// Calculate slippage in basis points.
    ///
    /// Formula: (avg_fill_price - submit_mid_price) / submit_mid_price * 10000
    /// For Buy orders: positive slippage = worse fill (paid more)
    /// For Sell orders: negative slippage = worse fill (received less)
    [[nodiscard]] static Price calculate_slippage_bps(Price fill_price, Price mid_price, Side side);
};

} // namespace pulse::execution
