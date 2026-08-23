#pragma once
// backtest_account.hpp — Lightweight virtual account / fill simulator (M29)
//
// The single-strategy MVP's order simulator: every non-Flat signal fills
// instantly at the signal price (the current candle close) with taker fees.
// No async, no book, no risk gate — replay-speed determinism beats execution
// realism here. PnL formulas mirror PositionManager::closePosition so a later
// "full-pipeline" fidelity mode produces identical numbers:
//   long:  (exit - entry) * qty * quanto
//   short: (entry - exit) * qty * quanto
//
// CloseMode::Flip (default): one position; an opposite signal closes it then
// opens the new direction; a same-direction signal is ignored and counted.
// CloseMode::Independent: at most one long and one short; a signal closes all
// positions of the opposite direction (then opens its own direction) while a
// same-direction open position ignores the signal.

#include "backtest/backtest_types.hpp"

#include <cstdint>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// BacktestAccount — position ledger + trade history for one replay
// ---------------------------------------------------------------------------
class BacktestAccount
{
  public:
    /// The option bundle defines quantity, quanto, fee rate and close mode.
    explicit BacktestAccount(const BacktestOptions &opts);

    /// Process one strategy signal. Fills at `sig.price` (the candle close
    /// the strategy observed). `candle_open_ms` timestamps the event.
    void onSignal(const strategy::TradingSignal &sig, std::int64_t candle_open_ms);

    /// Close every open position at `close_price` (window end).
    void closeAll(std::int64_t candle_open_ms, double close_price);

    /// Sample the equity curve at a candle boundary (realized + unrealized).
    void sampleEquity(std::int64_t candle_open_ms, double mark_price);

    [[nodiscard]] bool hasPosition() const;
    [[nodiscard]] double unrealizedPnL(double mark_price) const;
    [[nodiscard]] const std::vector<BacktestTrade> &trades() const;
    [[nodiscard]] const std::vector<EquityPoint> &equityCurve() const;

    /// Counters for the report header (see BacktestStats).
    [[nodiscard]] int entrySignalCount() const;
    [[nodiscard]] int ignoredSignalCount() const;
    [[nodiscard]] double realizedCash() const; ///< Sum of closed net PnL.

    /// Assemble the report statistics from trades + equity curve.
    [[nodiscard]] BacktestStats stats() const;

  private:
    struct OpenPosition
    {
        Side side = Side::Buy;
        double quantity = 0.0;
        double entry_price = 0.0;
        double entry_fee = 0.0;
        std::int64_t entry_open_ms = 0;
    };

    /// Open a position from a signal; returns true when a trade started.
    bool openFrom(const strategy::TradingSignal &sig, std::int64_t candle_open_ms);

    /// Close the position at `exit_price`, append a BacktestTrade.
    void closePosition(OpenPosition &pos, double exit_price, std::int64_t exit_open_ms);

    /// Close every open position of `side`.
    void closeSide(Side side, double exit_price, std::int64_t exit_open_ms);

    /// Position-side PnL of one position at a mark price.
    [[nodiscard]] double positionPnL(const OpenPosition &pos, double mark) const;

    [[nodiscard]] double feeFor(double price) const; ///< qty × price × quanto × rate.

    BacktestOptions m_opts;
    std::vector<OpenPosition> m_positions;
    std::vector<BacktestTrade> m_trades;
    std::vector<EquityPoint> m_equityCurve;
    int m_entrySignals = 0;
    int m_ignoredSignals = 0;
    double m_realizedCash = 0.0;
    std::int64_t m_tradeSeq = 0;
};

} // namespace pulse::backtest
