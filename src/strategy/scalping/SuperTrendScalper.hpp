#pragma once
// supertrend_scalper.hpp — SuperTrend indicator strategy (Layer 6 Strategy Engine)
//
// Detects trend direction changes using the SuperTrend indicator:
//   1. Computes ATR (Average True Range) over N periods from High/Low/Close
//   2. Builds upper and lower bands around the midpoint: (H + L) / 2 ± multiplier × ATR
//   3. Final bands are "tightened" — only move in the direction that favors the current trend
//   4. SuperTrend value = lower band when bullish, upper band when bearish
//   5. Buy signal  — price crosses above SuperTrend (trend flips bullish)
//   6. Sell signal — price crosses below SuperTrend (trend flips bearish)
//
// Confidence is derived from the distance between price and SuperTrend relative to ATR:
//   confidence = clamp(|close - supertrend| / atr, 0.0, 1.0)
//
// Data source:
//   - onKline() reads closed candles from KlineBuffer via the context
//   - Requires at least supertrend_period + 1 candles to produce a signal
//
// State commit contract (legacy semantics, preserved):
//   - Rolling band/trend state is committed on EVERY pass except the
//     atr<=0 early exit (nothing to compute on a flat market).
//   - A cooldown-blocked signal still commits state — the trend has moved
//     on even though no signal was emitted.
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - m_prev* fields are only written from the strategy thread
//   - m_params is atomic (inherited from StrategyParams)

#include "strategy/scalping/UnifiedScalper.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// SuperTrendScalper — ATR-based trend-following strategy
// ---------------------------------------------------------------------------
class SuperTrendScalper : public UnifiedScalper
{
  public:
    // Inherit the UnifiedScalper(context) constructor (public API unchanged).
    using UnifiedScalper::UnifiedScalper;

  protected:
    // --- UnifiedScalper hooks ---

    [[nodiscard]] std::string className() const override;
    [[nodiscard]] std::string idPrefix() const override;
    [[nodiscard]] std::size_t klineNeeded() const override;
    std::optional<EntryContext> evaluateEntry(
        const std::vector<market::Kline> &candles) override;
    void logSignal(const TradingSignal &sig) const override;

  private:
    double m_prevUpperBand{ 0.0 }; ///< Previous final upper band.
    double m_prevLowerBand{ 0.0 }; ///< Previous final lower band.
    double m_prevClose{ 0.0 };      ///< Previous candle close price.
    double m_prevSupertrend{ 0.0 }; ///< Previous SuperTrend value.
    bool m_isBullish{ false };      ///< Current trend direction (true = bullish).
    bool m_hasPrev{ false };        ///< Whether we have previous state for comparison.
};

} // namespace pulse::strategy
