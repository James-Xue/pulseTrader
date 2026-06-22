#pragma once
// strategy_params.hpp — Hot-reloadable strategy parameters (Layer 6 Strategy Engine)
//
// All tunable strategy parameters stored as std::atomic<double> so the AI layer
// (Phase 6, ParamAdvisor) can write updates lock-free while the strategy thread
// reads them on the hot path.
//
// Thread safety:
//   - AI thread writes with store(value, std::memory_order_release)
//   - Strategy thread reads with load(std::memory_order_acquire)
//   - No mutex, no lock — suitable for hot-path access

#include <atomic>
#include <cstdint>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// StrategyParams — atomic parameters shared between strategy and AI advisor
//
// Each field has a sensible default. Concrete strategies use a subset of these
// fields and may add strategy-specific atomic parameters in their own class.
//
// Fields:
//   1. order_quantity           — base order size in base currency
//   2. min_confidence           — minimum signal confidence to emit (0.0–1.0)
//   3. ema_fast_period          — fast EMA window size (for momentum strategies)
//   4. ema_slow_period          — slow EMA window size (for momentum strategies)
//   5. bb_period                — Bollinger Band window size
//   6. bb_std_dev               — Bollinger Band standard deviation multiplier
//   7. ob_imbalance_threshold   — order book imbalance threshold (0.0–1.0)
//   8. ob_depth                 — number of order book levels to analyze
//   9. cooldown_seconds         — minimum seconds between signals per symbol
//   10. stop_loss_pct           — stop-loss distance as fraction of entry price
//   11. take_profit_pct         — first take-profit target as fraction of entry
//   12. supertrend_period       — ATR period for SuperTrend bands
//   13. supertrend_multiplier   — ATR multiplier for SuperTrend bands
// ---------------------------------------------------------------------------
struct StrategyParams
{
    // --- Order sizing ---
    std::atomic<double> order_quantity{ 0.001 };       ///< Base order size (base currency).
    std::atomic<double> min_confidence{ 0.6 };         ///< Minimum confidence to emit.

    // --- Momentum (EMA crossover) ---
    std::atomic<double> ema_fast_period{ 9.0 };        ///< Fast EMA window.
    std::atomic<double> ema_slow_period{ 21.0 };       ///< Slow EMA window.

    // --- Mean reversion (Bollinger Bands) ---
    std::atomic<double> bb_period{ 20.0 };             ///< Bollinger Band window.
    std::atomic<double> bb_std_dev{ 2.0 };             ///< Standard deviation multiplier.

    // --- Order book scalping ---
    std::atomic<double> ob_imbalance_threshold{ 0.3 }; ///< Imbalance threshold (0.0–1.0).
    std::atomic<double> ob_depth{ 5.0 };               ///< Order book depth to analyze.

    // --- SuperTrend (ATR-based trend following) ---
    std::atomic<double> supertrend_period{ 10.0 };     ///< ATR period for SuperTrend.
    std::atomic<double> supertrend_multiplier{ 3.0 };  ///< ATR multiplier for SuperTrend bands.

    // --- Timing ---
    std::atomic<double> cooldown_seconds{ 30.0 };      ///< Seconds between signals per symbol.

    // --- Risk (AI-tunable, mirrors StopLossConfig / TakeProfitConfig) ---
    std::atomic<double> stop_loss_pct{ 0.01 };         ///< Stop-loss distance fraction.
    std::atomic<double> take_profit_pct{ 0.005 };       ///< First take-profit target fraction.
};

} // namespace pulse::strategy
