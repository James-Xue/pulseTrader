#pragma once
// snapshot_types.hpp — Cross-layer nlohmann to_json serialization (ADL)
//
// Single source of JSON serialization for domain types across layers:
//   pulse::market    — OrderBookLevel, Kline, Ticker, FeedStats
//   pulse::risk      — Position, PortfolioSummary, RiskSnapshot
//   pulse::execution — OrderSnapshot, ExecutionReport
//   pulse::strategy  — StrategySnapshot
//
// Each to_json() is defined in the type's own namespace so nlohmann's
// adl_serializer finds it via argument-dependent lookup.
//
// Also provides string helpers (sideToString / orderTypeToString /
// errorCodeToString) in pulse::snapshots.
//
// Thread safety:
//   - All structs are plain data (value types); callers are responsible
//     for serializing under the owning component's lock.

#include "ai/AnalysisResult.hpp"
#include "core/types.hpp"
#include "execution/ExecutionReport.hpp"
#include "execution/OrderTracker.hpp"
#include "market/KlineBuffer.hpp"
#include "market/MarketFeed.hpp"
#include "market/OrderBookManager.hpp"
#include "market/TickerCache.hpp"
#include "risk/risk_types.hpp"
#include "strategy/StrategyManager.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// to_json for pulse::market types — defined in pulse::market for ADL
// ---------------------------------------------------------------------------
namespace pulse::market
{

/// Serialize OrderBookLevel to JSON.
inline void to_json(nlohmann::json &j, const OrderBookLevel &level)
{
    j = nlohmann::json{
        {"price",    level.price},
        {"quantity", level.quantity},
    };
}

/// Serialize Kline to JSON.
inline void to_json(nlohmann::json &j, const Kline &k)
{
    j = nlohmann::json{
        {"open_time",  k.open_time},
        {"close_time", k.close_time},
        {"open",       k.open},
        {"high",       k.high},
        {"low",        k.low},
        {"close",      k.close},
        {"volume",     k.volume},
        {"closed",     k.closed},
    };
}

/// Serialize Ticker to JSON.
inline void to_json(nlohmann::json &j, const Ticker &t)
{
    j = nlohmann::json::object();
    j["symbol"] = t.symbol;
    j["last"] = t.last;
    j["bid"] = t.bid;
    j["ask"] = t.ask;
    j["volume_24h"] = t.volume_24h;
    j["change_pct"] = t.change_pct;
    j["timestamp"] = t.timestamp;
    j["mark_price"] = t.mark_price;
    j["index_price"] = t.index_price;
    j["funding_rate"] = t.funding_rate;
}

/// Serialize FeedStats to JSON.
inline void to_json(nlohmann::json &j, const FeedStats &fs)
{
    j = nlohmann::json{
        {"ticker_count",    fs.ticker_count},
        {"orderbook_count", fs.orderbook_count},
        {"kline_count",     fs.kline_count},
    };
}

} // namespace pulse::market

// ---------------------------------------------------------------------------
// to_json for pulse::risk types — defined in pulse::risk for ADL
// ---------------------------------------------------------------------------
namespace pulse::risk
{

/// Serialize Position to JSON.
inline void to_json(nlohmann::json &j, const Position &p)
{
    j = nlohmann::json::object();
    j["position_id"] = p.position_id;
    j["symbol"] = p.symbol;
    j["side"] = (Side::Buy == p.side) ? "buy" : "sell";
    j["quantity"] = p.quantity;
    j["entry_price"] = p.entry_price;
    j["current_price"] = p.current_price;
    j["unrealized_pnl"] = p.unrealized_pnl;
    j["notional_value"] = p.notional_value;
    j["open_time"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        p.open_time.time_since_epoch()).count();
    j["strategy_id"] = p.strategy_id;
    j["market_type"] = (MarketType::Futures == p.market_type)   ? "futures"
                     : (MarketType::Cfd == p.market_type) ? "cfd"
                                                          : "spot";
    j["leverage"] = p.leverage;
    j["margin_mode"] = (MarginMode::Cross == p.margin_mode) ? "cross" : "isolated";
    j["margin_used"] = p.margin_used;
    j["liquidation_price"] = p.liquidation_price;
    j["exchange_position_id"] = p.exchange_position_id;
    j["sl_price"] = p.sl_price;
    j["tp_price"] = p.tp_price;
}

/// Serialize PortfolioSummary to JSON.
inline void to_json(nlohmann::json &j, const PortfolioSummary &ps)
{
    j = nlohmann::json{
        {"openPositionCount",  ps.openPositionCount},
        {"total_notional",       ps.total_notional},
        {"total_unrealized_pnl", ps.total_unrealized_pnl},
        {"net_exposure",         ps.net_exposure},
    };
}

/// Serialize RiskSnapshot to JSON.
inline void to_json(nlohmann::json &j, const RiskSnapshot &snap)
{
    j = nlohmann::json::object();
    j["trading_halted"] = snap.trading_halted;
    j["haltReason"] = static_cast<std::uint32_t>(snap.haltReason);
    j["dailyDrawdown"] = snap.dailyDrawdown;
    j["maxDrawdown"] = snap.maxDrawdown;
    j["rate_limiter_tokens"] = snap.rate_limiter_tokens;
    j["rate_limiter_exhausted"] = snap.rate_limiter_exhausted;
    j["portfolio"] = snap.portfolio;
    j["openPositionCount"] = snap.openPositionCount;
}

} // namespace pulse::risk

// ---------------------------------------------------------------------------
// to_json for pulse::execution types — defined in pulse::execution for ADL
// ---------------------------------------------------------------------------
namespace pulse::execution
{

/// Convert OrderStatus to its string representation for JSON.
[[nodiscard]] inline std::string orderStatusToString(OrderStatus status)
{
    switch (status)
    {
    case OrderStatus::Pending:
        return "pending";
    case OrderStatus::Open:
        return "open";
    case OrderStatus::PartiallyFilled:
        return "open";
    case OrderStatus::Filled:
        return "filled";
    case OrderStatus::Cancelled:
        return "cancelled";
    case OrderStatus::Rejected:
        return "cancelled";
    default:
        return "pending";
    }
}

/// Serialize OrderSnapshot to JSON.
inline void to_json(nlohmann::json &j, const OrderSnapshot &o)
{
    j = nlohmann::json::object();
    j["order_id"] = o.order_id;
    j["symbol"] = o.symbol;
    j["side"] = (Side::Buy == o.side) ? "buy" : "sell";
    j["type"] = (OrderType::Market == o.type) ? "market"
              : (OrderType::Limit == o.type) ? "limit"
              : "post_only";
    j["requested_qty"] = o.requested_qty;
    j["filled_qty"] = o.filled_qty;
    j["status"] = orderStatusToString(o.status);
    j["submit_time"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        o.submit_time.time_since_epoch()).count();
    j["last_update_time"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        o.last_update_time.time_since_epoch()).count();
}

/// Serialize ExecutionReport to JSON (ADL wrapper).
///
/// Delegates to the ExecutionReport::to_json() member function.
inline void to_json(nlohmann::json &j, const ExecutionReport &r)
{
    j = r.to_json();
}

} // namespace pulse::execution

// ---------------------------------------------------------------------------
// to_json for pulse::strategy types — defined in pulse::strategy for ADL
// ---------------------------------------------------------------------------
namespace pulse::strategy
{

/// Serialize StrategySnapshot to JSON.
inline void to_json(nlohmann::json &j, const StrategySnapshot &s)
{
    j = nlohmann::json{
        {"name",             s.name},
        {"id",               s.id},
        {"symbol",           s.symbol},
        {"enabled",          s.enabled},
        {"running",          s.running},
        {"paused",           s.paused},
        {"poll_interval_ms", s.poll_interval_ms},
    };
}

} // namespace pulse::strategy

// ---------------------------------------------------------------------------
// String helpers — enum → string conversions for JSON/CLI output
// ---------------------------------------------------------------------------
namespace pulse::snapshots
{

/// Convert Side to lowercase string.
[[nodiscard]] inline std::string sideToString(Side side)
{
    return (Side::Buy == side) ? "buy" : "sell";
}

/// Convert OrderType to lowercase string.
[[nodiscard]] inline std::string orderTypeToString(OrderType type)
{
    switch (type)
    {
    case OrderType::Market:
        return "market";
    case OrderType::Limit:
        return "limit";
    case OrderType::PostOnly:
        return "post_only";
    default:
        return "market";
    }
}

/// Convert ErrorCode to a human-readable string (for halt reasons, errors).
[[nodiscard]] inline std::string errorCodeToString(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::Ok:
        return "ok";
    case ErrorCode::ManualHalt:
        return "manual_halt";
    case ErrorCode::DrawdownLimitHit:
        return "drawdown_limit_hit";
    default:
        return "error_" + std::to_string(static_cast<int>(code));
    }
}

} // namespace pulse::snapshots
