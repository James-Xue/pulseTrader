// backtest_report.cpp — Human-readable + JSON report for one backtest run (M29)

#include "backtest/BacktestReport.hpp"

#include "core/TimeUtil.hpp"

#include <fmt/format.h>

namespace pulse::backtest
{

namespace
{

/// Market label for the report header (e.g. "futures").
const char *marketLabel(MarketType mt)
{
    switch (mt)
    {
        case MarketType::Spot: return "spot";
        case MarketType::Futures: return "futures";
        case MarketType::Cfd: return "cfd";
    }
    return "unknown";
}

/// "YYYY-MM-DDTHH:MM:SS.mmm+00:00" from epoch ms (UTC — backtest windows
/// are data-time, never wall clock).
std::string isoUtc(std::int64_t epoch_ms)
{
    return pulse::formatEpochMs(epoch_ms, pulse::DisplayTimezone::utc());
}

std::string sideLabel(Side side)
{
    return (Side::Buy == side) ? "Buy" : "Sell";
}

std::string signalTypeLabel(strategy::SignalType type)
{
    switch (type)
    {
        case strategy::SignalType::Buy: return "Buy";
        case strategy::SignalType::Sell: return "Sell";
        case strategy::SignalType::Flat: return "Flat";
    }
    return "Flat";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BacktestReport::BacktestReport(const BacktestOptions &opts,
                               const BacktestStats &stats,
                               const KlineLoadStats &load,
                               const std::vector<BacktestSignal> &signals,
                               std::size_t warmup_candles,
                               std::size_t candles_fed)
    : m_opts{ opts }
    , m_stats{ stats }
    , m_load{ load }
    , m_signals{ signals }
    , m_warmupCandles{ warmup_candles }
    , m_candlesFed{ candles_fed }
{
}

// ---------------------------------------------------------------------------
// formatTable
// ---------------------------------------------------------------------------

std::string BacktestReport::formatTable() const
{
    const auto &s = m_stats;
    const auto &l = m_load;

    std::string out;
    out += fmt::format(
        "=== Backtest Report: {} / {} ({}) ===\n",
        m_opts.strategy_name, m_opts.symbol, marketLabel(m_opts.market_type));

    out += fmt::format(
        "Data      : {} candles (sqlite {} + api {}), {} .. {}\n",
        l.rows_total, l.rows_sqlite, l.rows_api,
        isoUtc(m_opts.from_ms), isoUtc(m_opts.to_ms));
    if (0 < l.missing_range_count)
    {
        out += fmt::format("  ({} missing range{})\n", l.missing_range_count,
                           (1 == l.missing_range_count) ? "" : "s");
    }
    for (const auto &w : l.warnings)
    {
        out += fmt::format("  warn: {}\n", w);
    }

    out += fmt::format("Candles   : {} fed, {} warmup\n", m_candlesFed, m_warmupCandles);
    out += fmt::format("Signals   : {} (entries {}, ignored {})\n",
                       s.signal_count, s.entry_signal_count, s.ignored_signal_count);
    out += fmt::format("Trades    : {} closed, {} open at end\n",
                       s.trade_count, s.open_at_end);

    out += fmt::format("Net PnL   : {:+.2f} USDT (gross +{:.2f} / -{:.2f}, fees -{:.2f})\n",
                       s.net_pnl, s.gross_profit, -s.gross_loss, s.total_fees);
    out += fmt::format("Win rate  : {:.1f}% ({}/{})\n", s.win_rate * 100.0,
                       static_cast<int>(s.win_rate * s.trade_count + 0.5), s.trade_count);
    out += fmt::format("Profit factor : {:.2f}\n", s.profit_factor);
    out += fmt::format("Max drawdown  : -{:.2f} USDT (-{:.1f}% of peak equity)\n",
                       s.max_drawdown, s.max_drawdown_pct);
    out += fmt::format("Largest win / loss : {:+.2f} / {:+.2f}\n",
                       s.largest_win, s.largest_loss);

    out += "Per-trade:\n";
    out += fmt::format("{:<4} {:<5} {:<5} {:<12} {:<12} {:>10} {:>10}\n",
                       "#", "side", "qty", "entry", "exit", "pnl", "return%");
    int idx = 1;
    for (const auto &t : s.trades)
    {
        out += fmt::format("{:<4} {:<5} {:<5.0f} {:<12.2f} {:<12.2f} {:>10.2f} {:>9.2f}%\n",
                           idx++, sideLabel(t.side), t.quantity,
                           t.entry_price, t.exit_price, t.net_pnl, t.return_pct);
    }
    return out;
}

// ---------------------------------------------------------------------------
// toJson
// ---------------------------------------------------------------------------

nlohmann::json BacktestReport::toJson() const
{
    const auto &s = m_stats;
    const auto &l = m_load;

    nlohmann::json j;
    j["strategy"] = m_opts.strategy_name;
    j["symbol"] = m_opts.symbol;
    j["market_type"] = marketLabel(m_opts.market_type);
    j["quantity"] = m_opts.order_quantity;
    j["quanto_multiplier"] = m_opts.quanto_multiplier;
    j["fee_rate"] = m_opts.taker_fee_rate;
    j["close_mode"] = (CloseMode::Flip == m_opts.close_mode) ? "flip" : "independent";

    j["range"] = {
        { "from_ms", m_opts.from_ms },
        { "to_ms", m_opts.to_ms },
        { "from", isoUtc(m_opts.from_ms) },
        { "to", isoUtc(m_opts.to_ms) },
    };

    j["data"] = {
        { "rows_sqlite", l.rows_sqlite },
        { "rows_api", l.rows_api },
        { "rows_total", l.rows_total },
        { "missing_ranges", l.missing_range_count },
        { "warnings", l.warnings },
    };

    j["warmup_candles"] = m_warmupCandles;
    j["candles_fed"] = m_candlesFed;

    nlohmann::json signals = nlohmann::json::array();
    for (const auto &sig : m_signals)
    {
        signals.push_back({
            { "candle_open_ms", sig.candle_open_ms },
            { "candle_open", isoUtc(sig.candle_open_ms) },
            { "type", signalTypeLabel(sig.type) },
            { "confidence", sig.confidence },
            { "price", sig.price },
            { "reason", sig.reason },
            { "indicators", sig.indicators },
        });
    }
    j["signals"] = signals;

    j["stats"] = {
        { "signal_count", s.signal_count },
        { "entry_signal_count", s.entry_signal_count },
        { "ignored_signal_count", s.ignored_signal_count },
        { "trade_count", s.trade_count },
        { "open_at_end", s.open_at_end },
        { "gross_profit", s.gross_profit },
        { "gross_loss", s.gross_loss },
        { "total_fees", s.total_fees },
        { "net_pnl", s.net_pnl },
        { "win_rate", s.win_rate },
        { "profit_factor", s.profit_factor },
        { "max_drawdown", s.max_drawdown },
        { "max_drawdown_pct", s.max_drawdown_pct },
        { "avg_win", s.avg_win },
        { "avg_loss", s.avg_loss },
        { "largest_win", s.largest_win },
        { "largest_loss", s.largest_loss },
    };

    nlohmann::json trades = nlohmann::json::array();
    for (const auto &t : s.trades)
    {
        trades.push_back({
            { "position_id", t.position_id },
            { "side", sideLabel(t.side) },
            { "quantity", t.quantity },
            { "quanto_multiplier", t.quanto_multiplier },
            { "entry_price", t.entry_price },
            { "exit_price", t.exit_price },
            { "entry_open_ms", t.entry_open_ms },
            { "exit_open_ms", t.exit_open_ms },
            { "entry_open", isoUtc(t.entry_open_ms) },
            { "exit_open", isoUtc(t.exit_open_ms) },
            { "entry_fee", t.entry_fee },
            { "exit_fee", t.exit_fee },
            { "pnl", t.pnl },
            { "net_pnl", t.net_pnl },
            { "return_pct", t.return_pct },
        });
    }
    j["trades"] = trades;

    nlohmann::json equity = nlohmann::json::array();
    for (const auto &pt : s.equity_curve)
    {
        equity.push_back({
            { "candle_open_ms", pt.candle_open_ms },
            { "equity", pt.equity },
        });
    }
    j["equity_curve"] = equity;

    return j;
}

} // namespace pulse::backtest
