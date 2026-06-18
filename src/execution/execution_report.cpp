// execution_report.cpp — ExecutionReport implementation (Layer 8 Order Execution)

#include "execution/execution_report.hpp"

#include <cmath>

namespace pulse::execution
{

nlohmann::json ExecutionReport::to_json() const
{
    return nlohmann::json{
        { "order_id", order_id },
        { "client_order_id", client_order_id },
        { "symbol", symbol },
        { "side", side == Side::Buy ? "buy" : "sell" },
        { "type",
            type == OrderType::Market   ? "market"
            : type == OrderType::Limit  ? "limit"
            : type == OrderType::PostOnly ? "post_only"
                                          : "unknown" },
        { "requested_qty", requested_qty },
        { "filled_qty", filled_qty },
        { "avg_fill_price", avg_fill_price },
        { "submit_mid_price", submit_mid_price },
        { "slippage_bps", slippage_bps },
        { "fees", fees },
        { "latency_ms", latency.count() },
        { "submit_time", submit_time.time_since_epoch().count() },
        { "fill_time", fill_time.time_since_epoch().count() },
        { "final_status",
            final_status == OrderStatus::Filled    ? "filled"
            : final_status == OrderStatus::Cancelled ? "cancelled"
                                                     : "unknown" }
    };
}

Price ExecutionReport::calculate_slippage_bps(Price fill_price, Price mid_price, Side side)
{
    if (0.0 == mid_price || 0.0 == fill_price)
    {
        return 0.0;
    }

    // Slippage = (fill_price - mid_price) / mid_price * 10000
    // For Buy: positive slippage means we paid more than mid (worse)
    // For Sell: negative slippage means we received less than mid (worse)
    const double raw_slippage = (fill_price - mid_price) / mid_price * 10000.0;

    // For Sell orders, invert the sign so positive = worse fill
    return (Side::Sell == side) ? -raw_slippage : raw_slippage;
}

} // namespace pulse::execution
