// SuperTrend.cpp — see the header for the algorithm and data contract

#include "risk/SuperTrend.hpp"

#include <algorithm>
#include <cmath>

namespace pulse::risk
{

std::optional<SuperTrendPoint> SuperTrend::compute(const std::vector<double> &highs,
    const std::vector<double> &lows,
    const std::vector<double> &closes,
    std::size_t period,
    double multiplier)
{
    // 1. Reject unusable inputs before touching any index:
    //    - period 0 would divide by zero in the ATR seed
    //    - mismatched series would silently read another series' candle
    //    - fewer than period + 1 candles cannot produce a single ATR value
    const std::size_t count = closes.size();
    if (0 == period || count < period + 1 || highs.size() != count || lows.size() != count)
    {
        return std::nullopt;
    }

    // 2. True range per bar. Index 0 has no previous close, so it is the
    //    plain high-low range; the ATR seed deliberately starts at index 1
    //    (same convention as the Qt client and Pine's ta.atr).
    std::vector<double> trueRanges(count, 0.0);
    trueRanges[0] = highs[0] - lows[0];

    for (std::size_t i = 1; i < count; ++i)
    {
        const double previousClose = closes[i - 1];
        trueRanges[i] =
            std::max({ highs[i] - lows[i], std::fabs(highs[i] - previousClose), std::fabs(lows[i] - previousClose) });
    }

    // 3. Seed the ATR as the SMA of the first `period` true ranges.
    double atr = 0.0;

    for (std::size_t i = 1; i <= period; ++i)
    {
        atr += trueRanges[i];
    }

    atr /= static_cast<double>(period);

    // 4. Walk the window while ratcheting the two bands. `direction` starts
    //    at 0 (undefined) and is decided on the first bar with an ATR.
    double finalUpper = 0.0;
    double finalLower = 0.0;
    int direction = 0;
    double previousClose = closes[0];

    for (std::size_t i = 1; i < count; ++i)
    {
        // 4a. Wilder smoothing after the seed bar.
        if (i > period)
        {
            const double n = static_cast<double>(period);
            atr = ((atr * (n - 1.0)) + trueRanges[i]) / n;
        }

        if (i >= period)
        {
            const double mid = (highs[i] + lows[i]) / 2.0;
            const double basicUpper = mid + (multiplier * atr);
            const double basicLower = mid - (multiplier * atr);

            if (0 == direction)
            {
                // 4b. First computable bar: both bands start at their basic
                //     values. Pine seeds the trend "up" (nz(trend[1], 1)) and
                //     flips down only when the close is below the same bar's
                //     lower band — not merely below the upper band.
                finalUpper = basicUpper;
                finalLower = basicLower;
                direction = (closes[i] < basicLower) ? -1 : 1;
            }
            else
            {
                // 4c. Ratchet: a band moves only in the favourable direction
                //     unless a close broke through it on the previous bar.
                const double previousUpper = finalUpper;
                const double previousLower = finalLower;

                finalUpper = (basicUpper < previousUpper || previousClose > previousUpper) ? basicUpper : previousUpper;
                finalLower = (basicLower > previousLower || previousClose < previousLower) ? basicLower : previousLower;

                // 4d. Flip on a close beyond the band the current trend rides:
                //     an uptrend ends below the previous LOWER band, and a
                //     downtrend ends above the previous UPPER band. (The Qt
                //     client implementation compares the opposite bands; that
                //     makes the direction alternate every bar in a steady
                //     trend, so the engine follows the canonical Pine
                //     formulation instead.)
                if (direction > 0 && closes[i] < previousLower)
                {
                    direction = -1;
                }
                else if (direction < 0 && closes[i] > previousUpper)
                {
                    direction = 1;
                }
            }
        }

        previousClose = closes[i];
    }

    // 5. The trend line rides the protective band: the lower band in an
    //    uptrend, the upper band in a downtrend.
    SuperTrendPoint point;
    if (0 != direction)
    {
        point.value = (direction > 0) ? finalLower : finalUpper;
        point.direction = direction;
    }
    return point;
}

} // namespace pulse::risk
