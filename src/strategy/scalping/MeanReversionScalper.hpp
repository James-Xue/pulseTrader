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
// Confidence is derived from how far price has penetrated beyond the band:
//   confidence = clamp(|price - band| / (upper - lower), 0.0, 1.0)
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

  private:
    StrategyParams m_params;

    std::int64_t m_lastSignalTimeMs{ 0 }; ///< Last signal timestamp (ms) for cooldown.
    std::int64_t m_lastWarmupLogMs{ 0 };  ///< Throttle warmup log to every 30 s.
    std::int64_t m_lastNoDataLogMs{ 0 }; ///< Throttle "no data" log to every 30 s.
};

} // namespace pulse::strategy
