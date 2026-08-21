#pragma once
// mean_reversion_scalper.hpp — Bollinger Band mean-reversion strategy (Layer 6 Strategy Engine)
//
// Detects overbought/oversold conditions using Bollinger Bands:
//   1. Computes SMA (simple moving average) over bb_period candles
//   2. Computes standard deviation over the same window
//   3. Upper band = SMA + bb_std_dev * stddev
//   4. Lower band = SMA - bb_std_dev * stddev
//   5. Buy signal  — price touches or falls below lower band (oversold)
//   6. Sell signal — price touches or rises above upper band (overbought)
//
// Confidence is derived from how far price has penetrated beyond the band,
// normalized by ATR14 (falls back to band width when ATR is unavailable):
//   confidence = clamp(|price - band| / ATR14, 0.0, 1.0)
// (ATR normalization keeps confidence meaningful across price scales —
// band-width ratio required ~60% of a 2σ band, near-unreachable on 1m gold)
//
// Data source:
//   - onKline() reads closed candles from KlineBuffer
//   - Requires at least bb_period candles to produce a signal
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - Stateless between candles (cooldown timestamp lives in UnifiedScalper)

#include "strategy/scalping/UnifiedScalper.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// MeanReversionScalper — Bollinger Band mean-reversion strategy
// ---------------------------------------------------------------------------
class MeanReversionScalper : public UnifiedScalper
{
  public:
    // Inherit the UnifiedScalper(context) constructor (public API unchanged).
    using UnifiedScalper::UnifiedScalper;

    /// Compute signal confidence from band penetration normalized by ATR.
    ///
    /// confidence = clamp(penetration / atr, 0.0, 1.0) when atr > 0,
    /// otherwise falls back to clamp(penetration / band_width, 0.0, 1.0).
    ///
    /// Parameters:
    ///   1. penetration — distance beyond the band (positive)
    ///   2. atr         — average true range (same price units)
    ///   3. band_width  — upper - lower band distance (fallback normalizer)
    [[nodiscard]] static double computeConfidence(double penetration,
        double atr,
        double band_width);

  protected:
    // --- UnifiedScalper hooks ---

    [[nodiscard]] std::string className() const override;
    [[nodiscard]] std::string idPrefix() const override;
    [[nodiscard]] std::size_t klineNeeded() const override;
    std::optional<EntryContext> evaluateEntry(
        const std::vector<market::Kline> &candles) override;
    void logSignal(const TradingSignal &sig) const override;
};

} // namespace pulse::strategy
