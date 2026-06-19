#pragma once
// trade_record.hpp — POD structs for trade recorder query results
//
// TradeRecord mirrors one row from the trades table.
// TradeSummary is an aggregate over a set of trades.

#include <cstdint>
#include <string>

namespace pulse::trade_recorder
{

// ---------------------------------------------------------------------------
// TradeRecord — one persisted trade (maps to a single row in `trades`)
// ---------------------------------------------------------------------------
struct TradeRecord
{
    std::int64_t id;
    std::string order_id;
    std::string client_order_id;
    std::int64_t timestamp_ns;   ///< Fill time as epoch nanoseconds.
    std::string symbol;
    std::string side;            ///< "buy" or "sell".
    std::string order_type;      ///< "market", "limit", or "post_only".
    double requested_qty;
    double filled_qty;
    double avg_fill_price;
    double submit_mid_price;
    double slippage_bps;
    double fees;
    double pnl;
    std::int64_t latency_ms;
    std::string final_status;    ///< "filled" or "cancelled".
    std::string strategy_name;
};

// ---------------------------------------------------------------------------
// TradeSummary — aggregate statistics over a set of trades
// ---------------------------------------------------------------------------
struct TradeSummary
{
    std::int64_t trade_count;
    double total_pnl;
    double total_fees;
    double win_rate;             ///< Fraction of trades with pnl > 0.
    double avg_slippage_bps;
    double avg_latency_ms;
    double total_volume;         ///< Sum of filled_qty * avg_fill_price.
};

} // namespace pulse::trade_recorder
