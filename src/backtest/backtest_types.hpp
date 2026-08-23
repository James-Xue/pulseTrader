#pragma once
// backtest_types.hpp — POD types shared across the backtest engine (M29)
//
// Central type definitions for historical strategy replay, following the
// multi-type header precedent of risk_types.hpp / signal_types.hpp
// (snake_case pure-data structs, no logic).

#include "core/PulseError.hpp"
#include "core/config.hpp"
#include "core/types.hpp"
#include "market/KlineBuffer.hpp"
#include "strategy/signal_types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// CloseMode — how a position is closed when the opposite signal arrives
// ---------------------------------------------------------------------------
enum class CloseMode : std::uint8_t
{
    Flip = 0,        ///< Close the open position, then open the opposite (default).
    Independent = 1, ///< Open the opposite without closing (multi-position model).
};

// ---------------------------------------------------------------------------
// BacktestOptions — one backtest run's full specification
//
// from_ms/to_ms == 0 means "auto-resolve from the data source's natural
// range" (resolved by BacktestEngine before loading).
// ---------------------------------------------------------------------------
struct BacktestOptions
{
    std::string strategy_name;          ///< Registry key, e.g. "ema_resonance_scalper".
    std::string symbol;                 ///< e.g. "ETH_USDT".
    MarketType market_type = MarketType::Futures;
    std::int64_t from_ms = 0;           ///< Candle open-time lower bound (0 = auto).
    std::int64_t to_ms = 0;             ///< Candle open-time upper bound (0 = auto).
    std::int64_t interval_ms = 60'000;  ///< Bar size; also gap detection + API pagination.
    double order_quantity = 0.0;        ///< 0 = take the config instance value.
    double min_confidence = 0.6;        ///< Signal confidence gate (seed, like live).
    double leverage = 1.0;              ///< Display only — does not affect PnL.
    double quanto_multiplier = 1.0;     ///< Futures contract size (ETH_USDT = 0.01).
    double taker_fee_rate = 0.0;        ///< <0 = no fees, 0 = market default
                                        ///< (futures 0.0005, spot 0.001), >0 = explicit.
    double cooldown_seconds = 0.0;      ///< 0 = replay disables wall-clock cooldown.
    CloseMode close_mode = CloseMode::Flip;
    bool api_backfill = true;           ///< Fetch missing ranges from Gate REST.
    bool cache_writeback = true;        ///< Persist API-fetched candles into kline_bars.
    std::string sqlite_db_path = "data/trades.db";
    std::string config_path;            ///< Optional trading.toml for instance params.
    std::string json_export_path;       ///< Optional JSON report export path.
};

// ---------------------------------------------------------------------------
// BacktestSignal — a signal captured during replay, timestamped by the candle
// that produced it (NOT by TradingSignal.timestamp, which is wall-clock
// nowMs() and meaningless under fast replay).
// ---------------------------------------------------------------------------
struct BacktestSignal
{
    std::int64_t candle_open_ms = 0;
    strategy::SignalType type = strategy::SignalType::Flat;
    double confidence = 0.0;
    double price = 0.0;
    std::string reason;
    nlohmann::json indicators = nlohmann::json::object();
};

// ---------------------------------------------------------------------------
// BacktestTrade — one closed round trip (entry → exit).
// ---------------------------------------------------------------------------
struct BacktestTrade
{
    std::string position_id;        ///< e.g. "bt_ETH_USDT_0001".
    Side side = Side::Buy;          ///< Position side (Buy = long).
    double quantity = 0.0;          ///< Contracts / base amount.
    double quanto_multiplier = 1.0;
    double entry_price = 0.0;
    double exit_price = 0.0;
    std::int64_t entry_open_ms = 0; ///< Candle open time of the entry signal.
    std::int64_t exit_open_ms = 0;  ///< Candle open time of the exit signal.
    double entry_fee = 0.0;
    double exit_fee = 0.0;
    double pnl = 0.0;               ///< Gross PnL (no fees).
    double net_pnl = 0.0;           ///< Gross PnL minus both fees.
    double return_pct = 0.0;        ///< Net PnL / notional.
};

// ---------------------------------------------------------------------------
// EquityPoint — one sample of the equity curve at a candle boundary.
// ---------------------------------------------------------------------------
struct EquityPoint
{
    std::int64_t candle_open_ms = 0;
    double equity = 0.0;
};

// ---------------------------------------------------------------------------
// BacktestStats — computed report statistics.
// ---------------------------------------------------------------------------
struct BacktestStats
{
    int signal_count = 0;            ///< All signals seen (incl. ignored).
    int entry_signal_count = 0;      ///< Signals that actually traded.
    int ignored_signal_count = 0;    ///< Same-direction repeats under Flip mode.
    int trade_count = 0;
    int open_at_end = 0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    double total_fees = 0.0;
    double net_pnl = 0.0;
    double win_rate = 0.0;           ///< 0..1.
    double profit_factor = 0.0;      ///< gross_profit / |gross_loss| (0 when no loss).
    double max_drawdown = 0.0;       ///< Absolute peak-to-trough equity drop.
    double max_drawdown_pct = 0.0;   ///< Relative to the peak before the drop.
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double largest_win = 0.0;
    double largest_loss = 0.0;
    std::vector<BacktestTrade> trades;
    std::vector<EquityPoint> equity_curve;
};

// ---------------------------------------------------------------------------
// KlineLoadStats — data loading diagnostics for the report header.
// ---------------------------------------------------------------------------
struct KlineLoadStats
{
    std::size_t rows_sqlite = 0;
    std::size_t rows_api = 0;
    std::size_t rows_total = 0;
    int missing_range_count = 0;
    std::vector<std::string> warnings;
};

// ---------------------------------------------------------------------------
// KlineLoadRequest — parameter bundle for KlineLoader::load.
// ---------------------------------------------------------------------------
struct KlineLoadRequest
{
    std::string symbol;
    MarketType market_type = MarketType::Futures;
    std::int64_t from_ms = 0;
    std::int64_t to_ms = 0;
    std::int64_t interval_ms = 60'000;
    bool api_backfill = true;
    bool cache_writeback = true;
};

} // namespace pulse::backtest
