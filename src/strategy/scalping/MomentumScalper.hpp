#pragma once
// momentum_scalper.hpp — EMA crossover strategy (Layer 6 Strategy Engine)
//
// Detects trend direction via Exponential Moving Average crossover:
//   1. Computes fast EMA (short window, e.g. 9 periods)
//   2. Computes slow EMA (long window, e.g. 21 periods)
//   3. Buy signal  — fast EMA crosses above slow EMA (bullish crossover)
//   4. Sell signal — fast EMA crosses below slow EMA (bearish crossover)
//
// Confidence is derived from the distance between EMAs normalized by ATR14:
//   confidence = clamp(|fast - slow| / ATR14, 0.0, 1.0)
// (ATR normalization keeps confidence comparable across price scales —
// dividing by price gave ~0.00007 for XAUUSD, always below min_confidence)
//
// Data source:
//   - onKline() reads closed candles from KlineBuffer via the context
//   - Requires at least ema_slow_period candles to produce a signal
//   - Note: this strategy has NO cooldown (legacy behavior preserved via
//     cooldownEnabled() = false — its cooldown_seconds param default is 30
//     but the legacy code never enforced it)
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - m_prevEmaFast / m_prevEmaSlow are only written from the strategy thread
//   - m_params is atomic (inherited from StrategyParams)

#include "strategy/scalping/UnifiedScalper.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// MomentumScalper — EMA crossover trend-following strategy
// ---------------------------------------------------------------------------
class MomentumScalper : public UnifiedScalper
{
  public:
    // Inherit the UnifiedScalper(context) constructor (public API unchanged:
    // tests construct MomentumScalper(ctx) directly).
    using UnifiedScalper::UnifiedScalper;

    /// Compute signal confidence from EMA separation normalized by ATR.
    ///
    /// confidence = clamp(|ema_fast - ema_slow| / atr, 0.0, 1.0)
    ///
    /// Parameters:
    ///   1. ema_fast — fast EMA value
    ///   2. ema_slow — slow EMA value
    ///   3. atr      — average true range (same price units); must be > 0
    ///
    /// Returns 0.0 when atr is non-positive (flat market, no conviction).
    [[nodiscard]] static double computeConfidence(double ema_fast, double ema_slow, double atr);

  protected:
    // --- UnifiedScalper hooks ---

    [[nodiscard]] std::string className() const override;
    [[nodiscard]] std::string idPrefix() const override;
    [[nodiscard]] std::size_t klineNeeded() const override;
    [[nodiscard]] std::size_t warmupThreshold() const override;
    [[nodiscard]] bool cooldownEnabled() const override;
    std::optional<EntryContext> evaluateEntry(
        const std::vector<market::Kline> &candles) override;

  private:
    double m_prevEmaFast{ 0.0 }; ///< Previous fast EMA value (for crossover detection).
    double m_prevEmaSlow{ 0.0 }; ///< Previous slow EMA value (for crossover detection).
    bool m_hasPrev{ false };      ///< Whether we have a previous EMA to compare against.
};

} // namespace pulse::strategy
