#pragma once
// ema_resonance_scalper.hpp — five-period EMA resonance strategy (Layer 6)
//
// Five-period FULL-ALIGNMENT resonance (default periods EMA 7/14/30/60/200):
//   - Bull resonance: ema7 > ema14 > ema30 > ema60 > ema200 (strict order)
//   - Bear resonance: ema7 < ema14 < ema30 < ema60 < ema200 (strict reverse)
//   - Otherwise (mixed / no strict ordering): no signal.
//
// Entry model — TRANSITION-triggered, not per-candle:
//   A Buy fires only when the resonance regime CHANGES into bull alignment
//   (mixed → bull, or a direct bear → bull flip); a Sell fires only on a
//   change into bear alignment. While an alignment persists, no repeat
//   signals. The template cooldown is a second, independent gate.
//
// Confidence — ATR-normalized total stack span:
//     confidence = clamp(|ema7 - ema200| / atr, 0, 1) × res_conf_scale
//   (span relative to volatility: strong consensus over a calm market scores
//   high; the same span in a choppy market scores low.)
//
// Custom parameters (TOML instance `custom_params = { ... }`, read once at
// construction — static, no hot reload):
//   res_ema_p1      (default 7)   — fastest EMA period
//   res_ema_p2      (default 14)
//   res_ema_p3      (default 30)
//   res_ema_p4      (default 60)
//   res_ema_p5      (default 200) — slowest EMA period
//   res_conf_scale  (default 1.0) — confidence scale factor
//
// EMA computation — fresh full-series recompute every candle:
//   computeEma(closes, period, 0.0) seeds SMA from the snapshot's first
//   `period` closes and recurses over the rest — deterministic, no rolling
//   per-EMA state to drift. klineNeeded = warmupThreshold = slowest period
//   + 1 (201 with defaults; the KlineBuffer backfills 500 on startup, so
//   warmup is seconds, not hours).
//
// No per-candle Flat state publishing in v1:
//   emitSignal drops confidence-0 signals below min_confidence, and the
//   grid trend gate reads a hardcoded "eth_scalper_" source — so resonance
//   state rides only on real Buy/Sell signals. (Extension point, documented
//   in docs/strategies/ema-resonance-scalper.md.)
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - m_prevResonance / m_hasPrev are only written from the strategy thread
//   - m_params is atomic (inherited from StrategyParams)

#include "strategy/scalping/UnifiedScalper.hpp"

#include <optional>
#include <string>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// EmaResonanceScalper — five-period full-alignment EMA resonance
// ---------------------------------------------------------------------------
class EmaResonanceScalper : public UnifiedScalper
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
    /// Resonance regime of the EMA stack.
    enum class Resonance
    {
        None, ///< No strict full ordering (mixed), or not yet evaluated.
        Bull, ///< ema7 > ema14 > ema30 > ema60 > ema200 (default periods).
        Bear, ///< ema7 < ema14 < ema30 < ema60 < ema200 (default periods).
    };

    Resonance m_prevResonance{ Resonance::None }; ///< Last committed regime.
    bool m_hasPrev{ false };                       ///< Any committed state yet.
};

} // namespace pulse::strategy
