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
//   - m_lastSignalTimeMs is only written from the strategy thread

#include "strategy/StrategyBase.hpp"

#include <cstdint>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// MeanReversionScalper — Bollinger Band mean-reversion strategy
// ---------------------------------------------------------------------------
class MeanReversionScalper : public StrategyBase
{
  public:
    /// Construct with injected context.
    explicit MeanReversionScalper(const StrategyContext &context);

    // --- StrategyBase overrides ---

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string id() const override;
    [[nodiscard]] StrategyParams &params() override;

    /// Called on each closed K-line candle.
    ///
    /// Computes Bollinger Bands from the last N candles and emits Buy/Sell
    /// signals when price touches or breaches the bands.
    void onKline(const market::Kline &kline) override;

    /// Called on ticker updates — not used by this strategy.
    void onTick(const market::Ticker &ticker) override;

    /// Called on orderbook updates — not used by this strategy.
    void onOrderbook(const market::OrderBook &book) override;

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

  private:
    StrategyParams m_params;

    std::int64_t m_lastSignalTimeMs{ 0 }; ///< Last signal timestamp (ms) for cooldown.
    std::int64_t m_lastWarmupLogMs{ 0 };  ///< Throttle warmup log to every 30 s.
    std::int64_t m_lastNoDataLogMs{ 0 }; ///< Throttle "no data" log to every 30 s.

    /// Compute ATR (average true range) over the last `period` candles.
    ///
    /// TR = max(high - low, |high - prev_close|, |low - prev_close|).
    /// Returns 0.0 when fewer than `period + 1` candles are available.
    [[nodiscard]] double computeAtr(const std::vector<market::Kline> &candles,
        std::size_t period) const;
};

} // namespace pulse::strategy
