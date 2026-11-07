// supertrend_scalper.cpp — SuperTrend indicator strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/SuperTrendScalper.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pulse::strategy
{

std::string SuperTrendScalper::className() const
{
    return "SuperTrendScalper";
}

std::string SuperTrendScalper::idPrefix() const
{
    return "supertrend_scalper";
}

std::size_t SuperTrendScalper::klineNeeded() const
{
    // Need period + 1 candles: period for ATR, +1 for the "previous close" of
    // the first TR.
    const auto period = static_cast<std::size_t>(
        m_params.supertrend_period.load(std::memory_order_acquire));
    return period + 1;
}

std::optional<EntryContext> SuperTrendScalper::evaluateEntry(
    const std::vector<market::Kline> &candles)
{
    // 1. Read hot-reloadable parameters (lock-free atomic loads).
    const auto period = static_cast<std::size_t>(
        m_params.supertrend_period.load(std::memory_order_acquire));
    const double multiplier = m_params.supertrend_multiplier.load(std::memory_order_acquire);

    // 2. Compute ATR.
    const double atr = computeAtr(candles, period);
    if (0.0 >= atr)
    {
        // ATR is zero — market is completely flat, skip. State NOT committed
        // (legacy: return happened before the state update).
        return std::nullopt;
    }

    // 3. Compute basic bands for the latest candle.
    const auto &latest = candles.back();
    const double midpoint = (latest.high + latest.low) / 2.0;
    const double basic_upper = midpoint + multiplier * atr;
    const double basic_lower = midpoint - multiplier * atr;

    // 4. Compute final bands (tightening logic).
    //    Upper band: take the lower of basic_upper and prev_upper (tighten upward moves).
    //    Reset if previous close broke above the previous upper band.
    double final_upper = basic_upper;
    if (m_hasPrev)
    {
        if (basic_upper < m_prevUpperBand || m_prevClose > m_prevUpperBand)
        {
            final_upper = basic_upper;
        }
        else
        {
            final_upper = m_prevUpperBand;
        }
    }

    //    Lower band: take the higher of basic_lower and prev_lower (tighten downward moves).
    //    Reset if previous close broke below the previous lower band.
    double final_lower = basic_lower;
    if (m_hasPrev)
    {
        if (basic_lower > m_prevLowerBand || m_prevClose < m_prevLowerBand)
        {
            final_lower = basic_lower;
        }
        else
        {
            final_lower = m_prevLowerBand;
        }
    }

    // 5. Determine current SuperTrend value and trend direction.
    bool current_bullish = m_isBullish;
    double current_supertrend = 0.0;

    if (m_hasPrev)
    {
        // If we were bullish and close stays above final_lower → stay bullish.
        if (m_isBullish && latest.close >= final_lower)
        {
            current_bullish = true;
            current_supertrend = final_lower;
        }
        // If we were bearish and close stays below final_upper → stay bearish.
        else if (!m_isBullish && latest.close <= final_upper)
        {
            current_bullish = false;
            current_supertrend = final_upper;
        }
        // Otherwise: trend flipped.
        else if (m_isBullish && latest.close < final_lower)
        {
            current_bullish = false;
            current_supertrend = final_upper;
        }
        else // !m_isBullish && close > final_upper
        {
            current_bullish = true;
            current_supertrend = final_lower;
        }
    }
    else
    {
        // First computation: infer trend from price position relative to midpoint.
        current_bullish = (latest.close >= midpoint);
        current_supertrend = current_bullish ? final_lower : final_upper;
    }

    // 6. Detect trend flip.
    std::optional<EntryContext> entry;
    if (m_hasPrev)
    {
        const bool flipped_bullish = !m_isBullish && current_bullish;
        const bool flipped_bearish = m_isBullish && !current_bullish;

        if (flipped_bullish || flipped_bearish)
        {
            // 7. Compute confidence: distance from price to SuperTrend, normalized by ATR.
            double confidence = std::abs(latest.close - current_supertrend) / atr;
            confidence = std::clamp(confidence, 0.0, 1.0);

            EntryContext e;
            e.type = flipped_bullish ? SignalType::Buy : SignalType::Sell;
            e.price = latest.close;
            e.confidence = confidence;
            e.atr = atr;
            e.reason = flipped_bullish
                ? "SuperTrend flipped bullish (price crossed above band)"
                : "SuperTrend flipped bearish (price crossed below band)";
            e.indicators = {
                { "supertrend", current_supertrend },
                { "supertrend_dir", flipped_bullish ? 1 : -1 },
                { "atr", atr },
            };
            entry = std::move(e);
        }
    }

    // 8. Store state for next candle. Committed unconditionally (even when a
    //    cooldown-blocked signal was suppressed) — legacy step 10 semantics.
    m_prevUpperBand = final_upper;
    m_prevLowerBand = final_lower;
    m_prevClose = latest.close;
    m_prevSupertrend = current_supertrend;
    m_isBullish = current_bullish;
    m_hasPrev = true;
    return entry;
}

void SuperTrendScalper::logSignal(const TradingSignal &sig) const
{
    PULSE_LOG_INFO("strategy",
        "[{}] {} signal: confidence={:.4f}, price={:.2f}, atr={:.2f}",
        id(), sig.reason, sig.confidence, sig.price,
        sig.indicators.value("atr", 0.0));
}

} // namespace pulse::strategy
