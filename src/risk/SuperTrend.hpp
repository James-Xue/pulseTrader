#pragma once
// SuperTrend.hpp — SuperTrend indicator for the gold dynamic-stop policy
//
// Layer 7 (Risk Management) helper. The gold trader compares its USD-based
// stop candidates with the 1-minute SuperTrend line, so the indicator lives
// next to its only consumer instead of the strategy layer.
//
// Algorithm (canonical Pine/TradingView formulation):
//   1. True range = max(high - low, |high - prev_close|, |low - prev_close|).
//   2. ATR seeds as the SMA of the first `period` true ranges, then uses
//      Wilder smoothing: ATR[i] = (ATR[i-1] * (n - 1) + TR[i]) / n.
//   3. Basic bands are hl2 +/- multiplier * ATR.
//   4. Final bands ratchet: the upper band only falls and the lower band only
//      rises unless a close breaks through them.
//   5. The trend starts on the first computable bar and flips when a close
//      crosses the previous bar's OWN trend band: an uptrend ends below the
//      lower band, a downtrend ends above the upper band.
//   6. The returned value is the band the trend rides: lower in an uptrend,
//      upper in a downtrend.
//
// Note: the Qt client's IndicatorEngine currently compares the opposite
// bands when flipping (its 4-bar unit test encodes that behaviour), which
// makes the direction alternate on every bar of a steady trend. The engine
// deliberately uses the canonical rule — the stop policy must not flip on
// noise. Aligning the client is tracked separately.
//
// The computation is stateless: callers pass a rolling window of candles and
// receive the newest point. That keeps the engine's per-second evaluation
// deterministic and trivially testable — no hidden per-symbol state to reset
// after a reconnect or a gap.
//
// Thread safety: pure static function over caller-owned vectors.

#include <cstddef>
#include <optional>
#include <vector>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// SuperTrendPoint — newest value and trend direction
// ---------------------------------------------------------------------------
struct SuperTrendPoint
{
    double value{ 0.0 }; ///< Band the trend rides (0 when undefined).
    int direction{ 0 };  ///< +1 = uptrend, -1 = downtrend, 0 = not enough bars.
};

// ---------------------------------------------------------------------------
// SuperTrend — stateless indicator over a rolling OHLC window
// ---------------------------------------------------------------------------
class SuperTrend
{
  public:
    /// Compute the newest SuperTrend point.
    ///
    /// Parameters:
    ///   1. highs      — candle highs, oldest first
    ///   2. lows       — candle lows, oldest first
    ///   3. closes     — candle closes, oldest first
    ///   4. period     — ATR period (10 by default)
    ///   5. multiplier — ATR band multiplier (3.0 by default)
    ///
    /// Returns nullopt when the window has fewer than period + 1 candles or
    /// the three series differ in length; returns direction 0 when no band
    /// could be computed. No exceptions are thrown on bad input.
    [[nodiscard]] static std::optional<SuperTrendPoint> compute(const std::vector<double> &highs,
        const std::vector<double> &lows,
        const std::vector<double> &closes,
        std::size_t period,
        double multiplier);
};

} // namespace pulse::risk
