// momentum_scalper.cpp — EMA crossover strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/MomentumScalper.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pulse::strategy
{

std::string MomentumScalper::className() const
{
    return "MomentumScalper";
}

std::string MomentumScalper::idPrefix() const
{
    return "momentum_scalper";
}

std::size_t MomentumScalper::klineNeeded() const
{
    // Enough candles for both EMAs (slow_period + 1 for the SMA seed) and
    // ATR14 confidence normalization (needs 15).
    const auto slow_period = static_cast<std::size_t>(
        m_params.ema_slow_period.load(std::memory_order_acquire));
    return std::max(slow_period + 1, std::size_t{ 15 });
}

std::size_t MomentumScalper::warmupThreshold() const
{
    // Legacy: signals only after slow_period candles (snapshot pulled more).
    return static_cast<std::size_t>(
        m_params.ema_slow_period.load(std::memory_order_acquire));
}

bool MomentumScalper::cooldownEnabled() const
{
    // Legacy Momentum never enforced a cooldown (its cooldown_seconds
    // default is 30 but the old code never checked it). Preserved exactly.
    return false;
}

std::optional<EntryContext> MomentumScalper::evaluateEntry(
    const std::vector<market::Kline> &candles)
{
    // 1. Read hot-reloadable parameters (lock-free atomic loads).
    const auto fast_period = static_cast<std::size_t>(
        m_params.ema_fast_period.load(std::memory_order_acquire));
    const auto slow_period = static_cast<std::size_t>(
        m_params.ema_slow_period.load(std::memory_order_acquire));

    // 2. Extract close prices from candles.
    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto &c : candles)
    {
        closes.push_back(c.close);
    }

    // 3. Compute fast and slow EMA.
    const double ema_fast = computeEma(closes, static_cast<double>(fast_period), m_prevEmaFast);
    const double ema_slow = computeEma(closes, static_cast<double>(slow_period), m_prevEmaSlow);

    // 4. Detect crossover (requires previous EMA values).
    std::optional<EntryContext> entry;
    if (m_hasPrev)
    {
        const bool bullish_cross = (m_prevEmaFast <= m_prevEmaSlow) && (ema_fast > ema_slow);
        const bool bearish_cross = (m_prevEmaFast >= m_prevEmaSlow) && (ema_fast < ema_slow);

        if (bullish_cross || bearish_cross)
        {
            // 5. Compute confidence: EMA separation normalized by ATR14.
            //    ATR normalization keeps confidence meaningful across price
            //    scales (dividing by price gave ~0.00007 for XAUUSD — always
            //    below min_confidence, so signals were silently dropped).
            const double atr = computeAtr(candles, 14);
            const double confidence = computeConfidence(ema_fast, ema_slow, atr);

            EntryContext e;
            e.type = bullish_cross ? SignalType::Buy : SignalType::Sell;
            e.price = closes.back();
            e.confidence = confidence;
            e.atr = atr;
            e.reason = bullish_cross
                ? "EMA bullish crossover (fast > slow)"
                : "EMA bearish crossover (fast < slow)";
            e.indicators = {
                { "ema_fast", ema_fast },
                { "ema_slow", ema_slow },
                { "ema_diff", ema_fast - ema_slow },
                { "atr", atr },
            };
            entry = std::move(e);
        }
    }

    // 6. Store current EMAs for next crossover detection.
    m_prevEmaFast = ema_fast;
    m_prevEmaSlow = ema_slow;
    m_hasPrev = true;
    return entry;
}

double MomentumScalper::computeConfidence(const double ema_fast, const double ema_slow, const double atr)
{
    if (atr <= 0.0)
    {
        return 0.0; // Flat market — no meaningful conviction.
    }
    return std::clamp(std::abs(ema_fast - ema_slow) / atr, 0.0, 1.0);
}

} // namespace pulse::strategy
