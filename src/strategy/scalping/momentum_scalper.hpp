#pragma once
// momentum_scalper.hpp — EMA crossover strategy (Layer 6 Strategy Engine)
//
// Detects trend direction via Exponential Moving Average crossover:
//   1. Computes fast EMA (short window, e.g. 9 periods)
//   2. Computes slow EMA (long window, e.g. 21 periods)
//   3. Buy signal  — fast EMA crosses above slow EMA (bullish crossover)
//   4. Sell signal — fast EMA crosses below slow EMA (bearish crossover)
//
// Confidence is derived from the distance between EMAs relative to price:
//   confidence = clamp(|fast - slow| / slow, 0.0, 1.0)
//
// Data source:
//   - onKline() reads closed candles from KlineBuffer via the context
//   - Requires at least ema_slow_period candles to produce a signal
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - m_prevEmaFast / m_prevEmaSlow are only written from the strategy thread
//   - m_params is atomic (inherited from StrategyParams)

#include "strategy/strategy_base.hpp"

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// MomentumScalper — EMA crossover trend-following strategy
// ---------------------------------------------------------------------------
class MomentumScalper : public StrategyBase
{
  public:
    /// Construct with injected context.
    ///
    /// Parameters:
    ///   1. context — dependency injection bundle (market feed, risk, executor)
    explicit MomentumScalper(const StrategyContext &context);

    // --- StrategyBase overrides ---

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string id() const override;
    [[nodiscard]] StrategyParams &params() override;

    /// Called on each closed K-line candle.
    ///
    /// Computes fast/slow EMA from the last N candles and detects crossovers.
    /// Emits Buy/Sell signals on crossover events.
    void onKline(const market::Kline &kline) override;

    /// Called on each ticker update — not used by this strategy (kline-driven).
    void onTick(const market::Ticker &ticker) override;

    /// Called on orderbook updates — not used by this strategy (kline-driven).
    void onOrderbook(const market::OrderBook &book) override;

  private:
    StrategyParams m_params;

    double m_prevEmaFast{ 0.0 }; ///< Previous fast EMA value (for crossover detection).
    double m_prevEmaSlow{ 0.0 }; ///< Previous slow EMA value (for crossover detection).
    bool m_hasPrev{ false };      ///< Whether we have a previous EMA to compare against.
    std::int64_t m_lastWarmupLogMs{ 0 }; ///< Throttle warmup log to every 30 s.
    std::int64_t m_lastNoDataLogMs{ 0 }; ///< Throttle "no data" log to every 30 s.

    /// Compute EMA from a series of close prices.
    ///
    /// EMA = price * k + prev_ema * (1 - k), where k = 2 / (period + 1)
    ///
    /// Parameters:
    ///   1. closes    — chronological close prices (oldest first)
    ///   2. period    — EMA window size
    ///   3. prev_ema  — previous EMA value (0.0 on first call → uses SMA seed)
    ///
    /// Returns the latest EMA value.
    [[nodiscard]] double computeEma(const std::vector<double> &closes,
        double period,
        double prev_ema) const;
};

} // namespace pulse::strategy
