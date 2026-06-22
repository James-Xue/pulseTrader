#pragma once
// snapshot_types.hpp — Per-panel snapshot structs + unified DashboardSnapshot (Layer 9 WebUI)
//
// Defines read-only snapshot structs for each WebUI dashboard panel:
//   1. OrderBookSnapshot   — top-of-book bids/asks per symbol
//   2. KlineSnapshot       — recent candlestick bars per symbol
//   3. PositionsSnapshot   — open positions + portfolio summary
//   4. OrdersSnapshot      — active orders + recent execution reports
//   5. MetricsSnapshot     — aggregate performance metrics (PnL, Sharpe, etc.)
//   6. AiSnapshot          — latest AI analysis result
//   7. StrategiesSnapshot  — registered strategy states
//   8. DashboardSnapshot   — unified aggregate of all panels
//
// Each struct has:
//   - A default constructor that zero-initializes all fields
//   - A nlohmann to_json() ADL function for JSON serialization
//
// ADL namespace placement:
//   to_json() for types from other layers is defined in the type's own namespace
//   so nlohmann's adl_serializer can find it via argument-dependent lookup.
//   to_json() for pulse::webui types is defined in pulse::webui.
//
// Thread safety:
//   - All structs are plain data (value types); thread safety is enforced
//     by DashboardState which holds them behind a shared_mutex.

#include "ai/analysis_result.hpp"
#include "core/types.hpp"
#include "execution/execution_report.hpp"
#include "execution/order_tracker.hpp"
#include "market/kline_buffer.hpp"
#include "market/orderbook_manager.hpp"
#include "risk/risk_types.hpp"
#include "strategy/strategy_manager.hpp"

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
}

/// Serialize PortfolioSummary to JSON.
inline void to_json(nlohmann::json &j, const PortfolioSummary &ps)
{
    j = nlohmann::json{
        {"open_position_count",  ps.open_position_count},
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
    j["halt_reason"] = static_cast<std::uint32_t>(snap.halt_reason);
    j["daily_drawdown"] = snap.daily_drawdown;
    j["max_drawdown"] = snap.max_drawdown;
    j["rate_limiter_tokens"] = snap.rate_limiter_tokens;
    j["rate_limiter_exhausted"] = snap.rate_limiter_exhausted;
    j["portfolio"] = snap.portfolio;
    j["open_position_count"] = snap.open_position_count;
}

} // namespace pulse::risk

// ---------------------------------------------------------------------------
// to_json for pulse::execution types — defined in pulse::execution for ADL
// ---------------------------------------------------------------------------
namespace pulse::execution
{

/// Convert OrderStatus to its string representation for JSON.
[[nodiscard]] inline std::string order_status_to_string(OrderStatus status)
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
    j["status"] = order_status_to_string(o.status);
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
        {"poll_interval_ms", s.poll_interval_ms},
    };
}

} // namespace pulse::strategy

// ---------------------------------------------------------------------------
// Snapshot structs + to_json for pulse::webui types
// ---------------------------------------------------------------------------
namespace pulse::webui
{

// ---------------------------------------------------------------------------
// OrderBookSnapshot — top N bids/asks for one symbol
// ---------------------------------------------------------------------------
struct OrderBookSnapshot
{
    Symbol symbol;                                    ///< Trading pair.
    std::vector<market::OrderBookLevel> bids;         ///< Top bids (highest first).
    std::vector<market::OrderBookLevel> asks;         ///< Top asks (lowest first).
    std::uint64_t sequence_id;                        ///< Exchange sequence number.
    std::int64_t timestamp;                           ///< Snapshot timestamp (ms).

    OrderBookSnapshot()
        : symbol{}
        , bids{}
        , asks{}
        , sequence_id{ 0 }
        , timestamp{ 0 }
    {
    }
};

// ---------------------------------------------------------------------------
// KlineSnapshot — recent candlestick bars for one symbol
// ---------------------------------------------------------------------------
struct KlineSnapshot
{
    Symbol symbol;                      ///< Trading pair.
    std::vector<market::Kline> candles; ///< Recent candles (oldest first).

    KlineSnapshot()
        : symbol{}
        , candles{}
    {
    }
};

// ---------------------------------------------------------------------------
// PositionsSnapshot — all open positions + portfolio summary
// ---------------------------------------------------------------------------
struct PositionsSnapshot
{
    std::vector<risk::Position> positions; ///< All open positions.
    risk::PortfolioSummary portfolio;      ///< Aggregated portfolio stats.

    PositionsSnapshot()
        : positions{}
        , portfolio{}
    {
    }
};

// ---------------------------------------------------------------------------
// OrdersSnapshot — active orders + recent execution reports
// ---------------------------------------------------------------------------
struct OrdersSnapshot
{
    std::vector<execution::OrderSnapshot> active_orders;    ///< Currently tracked orders.
    std::vector<execution::ExecutionReport> recent_reports; ///< Recent completed orders.

    OrdersSnapshot()
        : active_orders{}
        , recent_reports{}
    {
    }
};

// ---------------------------------------------------------------------------
// MetricsSnapshot — aggregate performance metrics
//
// Fields are zero-initialized; available=false means metrics have not been
// computed yet (e.g. no completed trades).
// ---------------------------------------------------------------------------
struct MetricsSnapshot
{
    double net_pnl;            ///< Net profit/loss across all trades.
    double gross_pnl;          ///< Gross profit/loss (before fees).
    double win_rate;           ///< Fraction of winning trades (0.0–1.0).
    double avg_win_loss_ratio; ///< Average win / average loss ratio.
    double sharpe_ratio;       ///< Annualized Sharpe ratio.
    double max_drawdown;       ///< Peak-to-valley drawdown fraction.
    int trade_count;           ///< Total number of completed trades.
    bool available;            ///< True if metrics have been computed.

    MetricsSnapshot()
        : net_pnl{ 0.0 }
        , gross_pnl{ 0.0 }
        , win_rate{ 0.0 }
        , avg_win_loss_ratio{ 0.0 }
        , sharpe_ratio{ 0.0 }
        , max_drawdown{ 0.0 }
        , trade_count{ 0 }
        , available{ false }
    {
    }
};

// ---------------------------------------------------------------------------
// AiSnapshot — latest AI analysis result
//
// available=false when no analysis cycle has completed yet.
// ---------------------------------------------------------------------------
struct AiSnapshot
{
    bool available;                          ///< True if an analysis result exists.
    ai::AnalysisResult result;               ///< Latest analysis result.
    std::int64_t last_update_ms;             ///< Wall-clock time of last update (ms).

    AiSnapshot()
        : available{ false }
        , result{}
        , last_update_ms{ 0 }
    {
    }
};

// ---------------------------------------------------------------------------
// StrategiesSnapshot — all registered strategy states
// ---------------------------------------------------------------------------
struct StrategiesSnapshot
{
    std::vector<strategy::StrategySnapshot> strategies; ///< All registered strategies.

    StrategiesSnapshot()
        : strategies{}
    {
    }
};

// ---------------------------------------------------------------------------
// AccountSnapshot — exchange-reported account balance
// ---------------------------------------------------------------------------
struct AccountSnapshot
{
    // Futures account
    bool available;             ///< Whether futures account data was successfully fetched.
    double total;               ///< Futures total equity.
    double available_balance;   ///< Futures available for new orders.
    double unrealised_pnl;      ///< Futures unrealized PnL.
    double position_margin;     ///< Futures margin locked in positions.
    double order_margin;        ///< Futures margin reserved for open orders.
    std::string currency;       ///< Settlement currency (e.g. "USDT").

    // Spot account
    bool spot_available;        ///< Whether spot account data was successfully fetched.
    double spot_total;          ///< Spot total balance.
    double spot_available_balance; ///< Spot available for trading.
    std::string spot_currency;  ///< Spot currency (e.g. "USDT").

    AccountSnapshot()
        : available{ false }
        , total{ 0.0 }
        , available_balance{ 0.0 }
        , unrealised_pnl{ 0.0 }
        , position_margin{ 0.0 }
        , order_margin{ 0.0 }
        , currency{}
        , spot_available{ false }
        , spot_total{ 0.0 }
        , spot_available_balance{ 0.0 }
        , spot_currency{}
    {
    }
};

// ---------------------------------------------------------------------------
// DashboardSnapshot — unified aggregate of all panel snapshots
//
// Produced by DashboardState::poll_loop() at tiered intervals.
// ---------------------------------------------------------------------------
struct DashboardSnapshot
{
    std::int64_t timestamp_ms;       ///< When this snapshot was assembled (ms).
    AccountSnapshot account;         ///< Account balance panel.
    OrderBookSnapshot order_book;    ///< Order book panel.
    KlineSnapshot kline;             ///< K-line panel.
    PositionsSnapshot positions;     ///< Positions panel.
    OrdersSnapshot orders;           ///< Orders panel.
    MetricsSnapshot metrics;         ///< Metrics panel.
    AiSnapshot ai;                   ///< AI analysis panel.
    StrategiesSnapshot strategies;   ///< Strategies panel.
    risk::RiskSnapshot risk;         ///< Risk management panel.

    DashboardSnapshot()
        : timestamp_ms{ 0 }
        , account{}
        , order_book{}
        , kline{}
        , positions{}
        , orders{}
        , metrics{}
        , ai{}
        , strategies{}
        , risk{}
    {
    }
};

// ---------------------------------------------------------------------------
// nlohmann to_json ADL functions for pulse::webui types
//
// These types live in pulse::webui, so their to_json is found via ADL
// when the argument type is a pulse::webui struct.
// ---------------------------------------------------------------------------

/// Serialize OrderBookSnapshot to JSON.
inline void to_json(nlohmann::json &j, const OrderBookSnapshot &snap)
{
    j = nlohmann::json::object();
    j["symbol"] = snap.symbol;
    j["bids"] = snap.bids;
    j["asks"] = snap.asks;
    j["sequence_id"] = snap.sequence_id;
    j["timestamp"] = snap.timestamp;
}

/// Serialize KlineSnapshot to JSON.
inline void to_json(nlohmann::json &j, const KlineSnapshot &snap)
{
    j = nlohmann::json::object();
    j["symbol"] = snap.symbol;
    j["candles"] = snap.candles;
}

/// Serialize PositionsSnapshot to JSON.
inline void to_json(nlohmann::json &j, const PositionsSnapshot &snap)
{
    j = nlohmann::json::object();
    j["positions"] = snap.positions;
    j["portfolio"] = snap.portfolio;
}

/// Serialize OrdersSnapshot to JSON.
inline void to_json(nlohmann::json &j, const OrdersSnapshot &snap)
{
    j = nlohmann::json::object();
    j["active_orders"] = snap.active_orders;
    j["recent_reports"] = snap.recent_reports;
}

/// Serialize MetricsSnapshot to JSON.
inline void to_json(nlohmann::json &j, const MetricsSnapshot &snap)
{
    j = nlohmann::json{
        {"net_pnl",            snap.net_pnl},
        {"gross_pnl",          snap.gross_pnl},
        {"win_rate",           snap.win_rate},
        {"avg_win_loss_ratio", snap.avg_win_loss_ratio},
        {"sharpe_ratio",       snap.sharpe_ratio},
        {"max_drawdown",       snap.max_drawdown},
        {"trade_count",        snap.trade_count},
        {"available",          snap.available},
    };
}

/// Serialize AiSnapshot to JSON.
inline void to_json(nlohmann::json &j, const AiSnapshot &snap)
{
    j = nlohmann::json::object();
    j["available"] = snap.available;
    j["result"] = snap.result;
    j["last_update_ms"] = snap.last_update_ms;
}

/// Serialize StrategiesSnapshot to JSON.
inline void to_json(nlohmann::json &j, const StrategiesSnapshot &snap)
{
    j = nlohmann::json::object();
    j["strategies"] = snap.strategies;
}

/// Serialize AccountSnapshot to JSON.
inline void to_json(nlohmann::json &j, const AccountSnapshot &snap)
{
    j = nlohmann::json::object();
    j["available"]         = snap.available;
    j["total"]             = snap.total;
    j["available_balance"] = snap.available_balance;
    j["unrealised_pnl"]    = snap.unrealised_pnl;
    j["position_margin"]   = snap.position_margin;
    j["order_margin"]      = snap.order_margin;
    j["currency"]          = snap.currency;
    j["spot_available"]         = snap.spot_available;
    j["spot_total"]             = snap.spot_total;
    j["spot_available_balance"] = snap.spot_available_balance;
    j["spot_currency"]          = snap.spot_currency;
}

/// Serialize DashboardSnapshot to JSON.
inline void to_json(nlohmann::json &j, const DashboardSnapshot &snap)
{
    j = nlohmann::json::object();
    j["timestamp_ms"] = snap.timestamp_ms;
    j["account"] = snap.account;
    j["order_book"] = snap.order_book;
    j["kline"] = snap.kline;
    j["positions"] = snap.positions;
    j["orders"] = snap.orders;
    j["metrics"] = snap.metrics;
    j["ai"] = snap.ai;
    j["strategies"] = snap.strategies;
    j["risk"] = snap.risk;
}

} // namespace pulse::webui
