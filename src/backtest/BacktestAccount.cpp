// backtest_account.cpp — Lightweight virtual account / fill simulator (M29)

#include "backtest/BacktestAccount.hpp"

#include <algorithm>
#include <limits>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BacktestAccount::BacktestAccount(const BacktestOptions &opts)
    : m_opts{ opts }
{
}

// ---------------------------------------------------------------------------
// PnL / fee helpers
// ---------------------------------------------------------------------------

double BacktestAccount::feeFor(double price) const
{
    // Fee base = notional = qty × price × quanto. Rate semantics (tri-state):
    // <0 disables fees, 0 falls back to the market default (futures taker
    // 0.05%, spot 0.1%), >0 is an explicit rate.
    double rate = m_opts.taker_fee_rate;
    if (rate < 0.0)
    {
        return 0.0;
    }
    if (rate == 0.0)
    {
        rate = (MarketType::Futures == m_opts.market_type) ? 0.0005 : 0.001;
    }
    return m_opts.order_quantity * price * m_opts.quanto_multiplier * rate;
}

double BacktestAccount::positionPnL(const OpenPosition &pos, double mark) const
{
    const double diff = (Side::Buy == pos.side)
        ? (mark - pos.entry_price)
        : (pos.entry_price - mark);
    return diff * pos.quantity * m_opts.quanto_multiplier;
}

// ---------------------------------------------------------------------------
// Signal processing
// ---------------------------------------------------------------------------

bool BacktestAccount::openFrom(const strategy::TradingSignal &sig,
                               std::int64_t candle_open_ms)
{
    if (m_opts.order_quantity <= 0.0)
    {
        // Unconfigured quantity — cannot trade. Treated as ignored.
        return false;
    }

    OpenPosition pos;
    pos.side = (strategy::SignalType::Buy == sig.type) ? Side::Buy : Side::Sell;
    pos.quantity = m_opts.order_quantity;
    pos.entry_price = sig.price;
    pos.entry_fee = feeFor(sig.price);
    pos.entry_open_ms = candle_open_ms;
    m_positions.push_back(pos);
    ++m_entrySignals;
    return true;
}

void BacktestAccount::closePosition(OpenPosition &pos, double exit_price,
                                    std::int64_t exit_open_ms)
{
    const double exit_fee = feeFor(exit_price);
    const double gross = positionPnL(pos, exit_price);
    const double net = gross - pos.entry_fee - exit_fee;

    BacktestTrade trade;
    trade.position_id = "bt_" + m_opts.symbol + "_" + std::to_string(m_tradeSeq);
    ++m_tradeSeq;
    trade.side = pos.side;
    trade.quantity = pos.quantity;
    trade.quanto_multiplier = m_opts.quanto_multiplier;
    trade.entry_price = pos.entry_price;
    trade.exit_price = exit_price;
    trade.entry_open_ms = pos.entry_open_ms;
    trade.exit_open_ms = exit_open_ms;
    trade.entry_fee = pos.entry_fee;
    trade.exit_fee = exit_fee;
    trade.pnl = gross;
    trade.net_pnl = net;

    const double notional = pos.quantity * pos.entry_price * m_opts.quanto_multiplier;
    trade.return_pct = (notional > 0.0) ? (net / notional) * 100.0 : 0.0;

    m_trades.push_back(trade);
    m_realizedCash += net;
}

void BacktestAccount::closeSide(Side side, double exit_price,
                                std::int64_t exit_open_ms)
{
    for (auto it = m_positions.begin(); it != m_positions.end();)
    {
        if (side == it->side)
        {
            closePosition(*it, exit_price, exit_open_ms);
            it = m_positions.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void BacktestAccount::onSignal(const strategy::TradingSignal &sig,
                               std::int64_t candle_open_ms)
{
    if (strategy::SignalType::Flat == sig.type || sig.price <= 0.0)
    {
        return; // Status-channel signals are not trades.
    }

    const bool buy_signal = (strategy::SignalType::Buy == sig.type);
    const Side signal_side = buy_signal ? Side::Buy : Side::Sell;

    const bool has_same = std::any_of(m_positions.begin(), m_positions.end(),
        [signal_side](const OpenPosition &p) { return p.side == signal_side; });

    if (has_same)
    {
        // Same-direction repeat: ignored in both modes (Flip holds one
        // position; Independent caps at one per direction).
        ++m_ignoredSignals;
        return;
    }

    // Close all positions of the opposite direction — Flip: the single one,
    // then re-open the new direction; Independent: all of that side.
    closeSide(buy_signal ? Side::Sell : Side::Buy, sig.price, candle_open_ms);

    // Open the new position; a signal that cannot trade (e.g. unconfigured
    // quantity) is counted as ignored.
    if (!openFrom(sig, candle_open_ms))
    {
        ++m_ignoredSignals;
    }
}

void BacktestAccount::closeAll(std::int64_t candle_open_ms, double close_price)
{
    // Copy and clear so closePosition's pushes never alias erased elements.
    auto positions = m_positions;
    m_positions.clear();
    for (auto &pos : positions)
    {
        closePosition(pos, close_price, candle_open_ms);
    }
}

void BacktestAccount::sampleEquity(std::int64_t candle_open_ms, double mark_price)
{
    EquityPoint pt;
    pt.candle_open_ms = candle_open_ms;
    pt.equity = m_realizedCash + unrealizedPnL(mark_price);
    m_equityCurve.push_back(pt);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool BacktestAccount::hasPosition() const
{
    return !m_positions.empty();
}

double BacktestAccount::unrealizedPnL(double mark_price) const
{
    double total = 0.0;
    for (const auto &pos : m_positions)
    {
        total += positionPnL(pos, mark_price);
    }
    return total;
}

const std::vector<BacktestTrade> &BacktestAccount::trades() const
{
    return m_trades;
}

const std::vector<EquityPoint> &BacktestAccount::equityCurve() const
{
    return m_equityCurve;
}

int BacktestAccount::entrySignalCount() const
{
    return m_entrySignals;
}

int BacktestAccount::ignoredSignalCount() const
{
    return m_ignoredSignals;
}

double BacktestAccount::realizedCash() const
{
    return m_realizedCash;
}

// ---------------------------------------------------------------------------
// stats — report statistics from trades + equity curve
// ---------------------------------------------------------------------------

BacktestStats BacktestAccount::stats() const
{
    BacktestStats s;
    s.entry_signal_count = m_entrySignals;
    s.ignored_signal_count = m_ignoredSignals;
    s.signal_count = m_entrySignals + m_ignoredSignals;
    s.trade_count = static_cast<int>(m_trades.size());
    s.open_at_end = static_cast<int>(m_positions.size());

    int wins = 0;
    for (const auto &t : m_trades)
    {
        s.total_fees += t.entry_fee + t.exit_fee;
        if (t.net_pnl >= 0.0)
        {
            ++wins;
            s.gross_profit += t.net_pnl;
            s.avg_win += t.net_pnl;
            s.largest_win = std::max(s.largest_win, t.net_pnl);
        }
        else
        {
            s.gross_loss += t.net_pnl; // negative
            s.avg_loss += t.net_pnl;
            s.largest_loss = std::min(s.largest_loss, t.net_pnl);
        }
    }
    s.net_pnl = m_realizedCash;

    if (0 < s.trade_count)
    {
        s.win_rate = static_cast<double>(wins) / static_cast<double>(s.trade_count);
        s.avg_win /= wins;
        s.avg_loss /= (s.trade_count - wins);
    }

    // Profit factor: gross wins / |gross losses|. 0 when there were no losing
    // trades (perfect score), reported as-is when there were no wins.
    if (s.gross_loss < 0.0)
    {
        s.profit_factor = s.gross_profit / (-s.gross_loss);
    }
    else if (s.gross_profit > 0.0)
    {
        s.profit_factor = std::numeric_limits<double>::infinity();
    }

    // Max drawdown over the equity curve (peak → trough, absolute + pct).
    // The peak starts at the FIRST point, so an all-negative curve still
    // reports its absolute drawdown; the pct needs a non-zero peak to be
    // meaningful (stays 0 otherwise).
    double peak = m_equityCurve.empty() ? 0.0 : m_equityCurve.front().equity;
    for (const auto &pt : m_equityCurve)
    {
        peak = std::max(peak, pt.equity);
        const double dd = peak - pt.equity;
        s.max_drawdown = std::max(s.max_drawdown, dd);
        if (0.0 != peak)
        {
            s.max_drawdown_pct = std::max(s.max_drawdown_pct,
                (dd / peak) * 100.0);
        }
    }

    s.trades = m_trades;
    s.equity_curve = m_equityCurve;
    return s;
}

} // namespace pulse::backtest
