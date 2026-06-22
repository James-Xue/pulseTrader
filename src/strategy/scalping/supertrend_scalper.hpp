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
//   - on_kline() reads closed candles from KlineBuffer via the context
//   - Requires at least supertrend_period + 1 candles to produce a signal
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - prev_* fields are only written from the strategy thread
//   - params_ is atomic (inherited from StrategyParams)

#include "strategy/strategy_base.hpp"

#include <cstdint>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// SuperTrendScalper — ATR-based trend-following strategy
// ---------------------------------------------------------------------------
class SuperTrendScalper : public StrategyBase
{
  public:
    /// Construct with injected context.
    explicit SuperTrendScalper(const StrategyContext &context);

    // --- StrategyBase overrides ---

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string id() const override;
    [[nodiscard]] StrategyParams &params() override;

    /// Called on each closed K-line candle.
    ///
    /// Computes the SuperTrend indicator from historical candles and detects
    /// trend flips (bullish ↔ bearish crossover).
    void on_kline(const market::Kline &kline) override;

    /// Called on each ticker update — not used by this strategy (kline-driven).
    void on_tick(const market::Ticker &ticker) override;

    /// Called on orderbook updates — not used by this strategy (kline-driven).
    void on_orderbook(const market::OrderBook &book) override;

  private:
    StrategyParams params_;

    double prev_upper_band_{ 0.0 }; ///< Previous final upper band.
    double prev_lower_band_{ 0.0 }; ///< Previous final lower band.
    double prev_close_{ 0.0 };      ///< Previous candle close price.
    double prev_supertrend_{ 0.0 }; ///< Previous SuperTrend value.
    bool is_bullish_{ false };      ///< Current trend direction (true = bullish).
    bool has_prev_{ false };        ///< Whether we have previous state for comparison.
    std::int64_t last_signal_time_ms_{ 0 };  ///< Cooldown enforcement.
    std::int64_t last_warmup_log_ms_{ 0 };   ///< Throttle warmup log to every 30 s.
    std::int64_t last_no_data_log_ms_{ 0 };  ///< Throttle "no data" log to every 30 s.

    /// Compute ATR (Average True Range) from a series of candles.
    ///
    /// True Range = max(high - low, |high - prev_close|, |low - prev_close|)
    /// ATR = average of the last `period` true ranges.
    ///
    /// Parameters:
    ///   1. candles — chronological candle data (oldest first)
    ///   2. period  — ATR window size
    ///
    /// Returns the ATR value, or 0.0 if insufficient data.
    [[nodiscard]] double compute_atr(const std::vector<market::Kline> &candles,
        std::size_t period) const;
};

} // namespace pulse::strategy
