#pragma once
// orderbook_scalper.hpp — Order book imbalance strategy (Layer 6 Strategy Engine)
//
// Detects short-term directional bias from order book pressure:
//   1. Sums bid volume (buy pressure) across top N levels
//   2. Sums ask volume (sell pressure) across top N levels
//   3. Computes imbalance = (bid_vol - ask_vol) / (bid_vol + ask_vol)
//   4. Buy signal  — imbalance > threshold (buyers dominate)
//   5. Sell signal — imbalance < -threshold (sellers dominate)
//
// Confidence is the absolute imbalance value (already in [0.0, 1.0]).
//
// Data source:
//   - on_orderbook() reads the current order book snapshot
//   - Depth is configurable via params_.ob_depth
//   - Threshold is configurable via params_.ob_imbalance_threshold
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - last_signal_time_ is only written from the strategy thread

#include "strategy/strategy_base.hpp"

#include <cstdint>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// OrderBookScalper — order book imbalance scalping strategy
// ---------------------------------------------------------------------------
class OrderBookScalper : public StrategyBase
{
  public:
    /// Construct with injected context.
    explicit OrderBookScalper(const StrategyContext &context);

    // --- StrategyBase overrides ---

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string id() const override;
    [[nodiscard]] StrategyParams &params() override;

    /// Called on orderbook updates — the primary trigger for this strategy.
    ///
    /// Computes bid/ask volume imbalance and emits Buy/Sell signals
    /// when the imbalance exceeds the configured threshold.
    void on_orderbook(const market::OrderBook &book) override;

    /// Called on ticker updates — not used by this strategy.
    void on_tick(const market::Ticker &ticker) override;

    /// Called on kline close — not used by this strategy.
    void on_kline(const market::Kline &kline) override;

  private:
    StrategyParams params_;

    std::int64_t last_signal_time_ms_{ 0 }; ///< Last signal timestamp (ms) for cooldown.
};

} // namespace pulse::strategy
