#pragma once
// eth_scalper.hpp — ETH-specific short strategy (Layer 6 Strategy Engine)
//
// First coin-specialized strategy, built on the UnifiedScalper template.
// Demonstrates the three coin-extension mechanisms:
//   1. Custom entry logic — short-only: emits ONLY on a bearish EMA
//      crossover (fast crosses below slow). Bullish crossovers are ignored
//      (ETH chase-short regime).
//   2. Custom parameters (TOML instance `custom_params = { ... }`, read via
//      UnifiedScalper::customParam):
//        eth_atr_step            (default 0.05) — ATR-adaptive suggested TP:
//                                suggested_tp = close - eth_atr_step * atr
//                                (big ATR → wider TP, small ATR → tighter TP)
//        eth_spike_filter_usd    (default 120.0) — a candle whose high-low
//                                range exceeds this is a 暴拉/插针 spike:
//                                do NOT chase it short
//        eth_min_confidence_scale (default 1.0) — confidence scale factor
//   3. Custom confidence — ATR-normalized EMA separation, then scaled by
//      eth_min_confidence_scale and clamped to [0, 1].
//
// Note: this is intentionally NOT a full grid implementation — it validates
// the extension mechanism with a live short setup; grid logic can follow in
// a later iteration.
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - m_prevEma* are only written from the strategy thread
//   - m_params is atomic (inherited from StrategyParams)

#include "strategy/scalping/UnifiedScalper.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// EthScalper — ETH short-only EMA crossover with spike filter
// ---------------------------------------------------------------------------
class EthScalper : public UnifiedScalper
{
  public:
    using UnifiedScalper::UnifiedScalper;

  protected:
    // --- UnifiedScalper hooks ---

    [[nodiscard]] std::string className() const override;
    [[nodiscard]] std::string idPrefix() const override;
    [[nodiscard]] std::size_t klineNeeded() const override;
    [[nodiscard]] std::size_t warmupThreshold() const override;
    std::optional<EntryContext> evaluateEntry(
        const std::vector<market::Kline> &candles) override;

  private:
    double m_prevEmaFast{ 0.0 }; ///< Previous fast EMA (for crossover detection).
    double m_prevEmaSlow{ 0.0 }; ///< Previous slow EMA (for crossover detection).
    bool m_hasPrev{ false };      ///< Whether we have a previous EMA to compare against.
};

} // namespace pulse::strategy
