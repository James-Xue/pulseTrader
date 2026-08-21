// eth_scalper.cpp — ETH-specific short strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/EthScalper.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pulse::strategy
{

std::string EthScalper::className() const
{
    return "EthScalper";
}

std::string EthScalper::idPrefix() const
{
    return "eth_scalper";
}

std::size_t EthScalper::klineNeeded() const
{
    // Enough candles for both EMAs (slow_period + 1 for the SMA seed) and
    // ATR14 confidence normalization (needs 15).
    const auto slow_period = static_cast<std::size_t>(
        m_params.ema_slow_period.load(std::memory_order_acquire));
    return std::max(slow_period + 1, std::size_t{ 15 });
}

std::size_t EthScalper::warmupThreshold() const
{
    // Signals only after slow_period candles (snapshot pulls more).
    return static_cast<std::size_t>(
        m_params.ema_slow_period.load(std::memory_order_acquire));
}

std::optional<EntryContext> EthScalper::evaluateEntry(
    const std::vector<market::Kline> &candles)
{
    // 1. Read hot-reloadable parameters (lock-free atomic loads).
    const auto fast_period = static_cast<std::size_t>(
        m_params.ema_fast_period.load(std::memory_order_acquire));
    const auto slow_period = static_cast<std::size_t>(
        m_params.ema_slow_period.load(std::memory_order_acquire));

    // 2. Coin-specific parameters (static, from the TOML custom_params table).
    const double atr_step = customParam("eth_atr_step", 0.05);
    const double spike_filter_usd = customParam("eth_spike_filter_usd", 120.0);
    const double conf_scale = customParam("eth_min_confidence_scale", 1.0);

    // 3. Extract close prices and compute fast/slow EMA.
    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto &c : candles)
    {
        closes.push_back(c.close);
    }

    const double ema_fast = computeEma(closes, static_cast<double>(fast_period), m_prevEmaFast);
    const double ema_slow = computeEma(closes, static_cast<double>(slow_period), m_prevEmaSlow);

    // 4. Short-only entry: detect the BEARISH crossover. Bullish crossovers
    //    are ignored (chase-short regime).
    std::optional<EntryContext> entry;
    if (m_hasPrev)
    {
        const bool bearish_cross = (m_prevEmaFast >= m_prevEmaSlow) && (ema_fast < ema_slow);

        if (bearish_cross)
        {
            const auto &latest = candles.back();

            // 5. Spike filter: a candle whose high-low range exceeds the
            //    threshold is a 暴拉/插针 — do NOT chase it short.
            const double range_usd = latest.high - latest.low;
            if (range_usd <= spike_filter_usd)
            {
                // 6. Confidence: ATR-normalized EMA separation, scaled by
                //    the coin-specific factor, clamped to [0, 1].
                const double atr = computeAtr(candles, 14);
                if (atr > 0.0)
                {
                    double confidence = std::clamp(
                        std::abs(ema_fast - ema_slow) / atr, 0.0, 1.0);
                    confidence = std::clamp(confidence * conf_scale, 0.0, 1.0);

                    EntryContext e;
                    e.type = SignalType::Sell;
                    e.price = latest.close;
                    e.confidence = confidence;
                    e.atr = atr;
                    e.reason = "ETH short setup: EMA bearish crossover (fast < slow)";
                    e.indicators = {
                        { "ema_fast", ema_fast },
                        { "ema_slow", ema_slow },
                        { "atr", atr },
                        { "atr_step", atr_step },
                        { "suggested_tp", latest.close - atr_step * atr },
                        { "spike_range_usd", range_usd },
                        { "spike_filter_usd", spike_filter_usd },
                    };
                    entry = std::move(e);
                }
            }
        }
    }

    // 7. Store current EMAs for next crossover detection (committed even
    //    when no signal fired — the trend has moved on regardless).
    m_prevEmaFast = ema_fast;
    m_prevEmaSlow = ema_slow;
    m_hasPrev = true;
    return entry;
}

} // namespace pulse::strategy
