// ema_resonance_scalper.cpp — five-period EMA resonance strategy (Layer 6)

#include "strategy/scalping/EmaResonanceScalper.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pulse::strategy
{

std::string EmaResonanceScalper::className() const
{
    return "EmaResonanceScalper";
}

std::string EmaResonanceScalper::idPrefix() const
{
    return "ema_resonance_scalper";
}

std::size_t EmaResonanceScalper::klineNeeded() const
{
    // The slowest EMA (default 200) needs `period` closes for its SMA seed
    // plus 1 current candle. The period is configurable via custom_params,
    // so read the live value.
    return static_cast<std::size_t>(customParam("res_ema_p5", 200.0)) + 1;
}

std::size_t EmaResonanceScalper::warmupThreshold() const
{
    // No signal until the whole stack (incl. the slowest EMA) is computable.
    return klineNeeded();
}

std::optional<EntryContext> EmaResonanceScalper::evaluateEntry(
    const std::vector<market::Kline> &candles)
{
    // 1. Read the five periods + confidence scale (static coin-specific
    //    parameters from the TOML custom_params table).
    const double p1 = customParam("res_ema_p1", 7.0);
    const double p2 = customParam("res_ema_p2", 14.0);
    const double p3 = customParam("res_ema_p3", 30.0);
    const double p4 = customParam("res_ema_p4", 60.0);
    const double p5 = customParam("res_ema_p5", 200.0);
    const double conf_scale = customParam("res_conf_scale", 1.0);

    // 2. Extract close prices.
    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto &c : candles)
    {
        closes.push_back(c.close);
    }

    // 3. Fresh full-series computation for all five EMAs (prev = 0.0 →
    //    SMA-seeded from the snapshot). Deterministic — no rolling state.
    const double e1 = computeEma(closes, p1, 0.0);
    const double e2 = computeEma(closes, p2, 0.0);
    const double e3 = computeEma(closes, p3, 0.0);
    const double e4 = computeEma(closes, p4, 0.0);
    const double e5 = computeEma(closes, p5, 0.0);

    // 4. Resonance regime — strict full ordering (all five aligned).
    Resonance res = Resonance::None;
    if (e1 > e2 && e2 > e3 && e3 > e4 && e4 > e5)
    {
        res = Resonance::Bull;
    }
    else if (e1 < e2 && e2 < e3 && e3 < e4 && e4 < e5)
    {
        res = Resonance::Bear;
    }

    // 5. Transition trigger: fire only when the regime CHANGES into an
    //    alignment (None → Bull/Bear, or a direct Bull ↔ Bear flip).
    const bool transition = m_hasPrev && res != m_prevResonance
                            && res != Resonance::None;

    // 6. Commit the regime unconditionally (the trend has moved regardless).
    m_prevResonance = res;
    m_hasPrev = true;

    // 7. No entry without a fresh alignment — state already committed above
    //    (template contract: cooldown must not undo a committed state).
    if (!transition)
    {
        return std::nullopt;
    }

    // 8. Confidence: ATR-normalized total stack span, scaled, clamped.
    const double atr = computeAtr(candles, 14);
    if (atr <= 0.0)
    {
        return std::nullopt;
    }

    EntryContext e;
    e.price = candles.back().close;
    e.atr = atr;
    e.indicators = {
        { "ema7", e1 },  { "ema14", e2 }, { "ema30", e3 },
        { "ema60", e4 }, { "ema200", e5 },
        { "resonance", (Resonance::Bull == res) ? "bull_aligned"
                       : (Resonance::Bear == res) ? "bear_aligned"
                                                  : "mixed" },
        { "atr", atr },
    };

    e.type = (Resonance::Bull == res) ? SignalType::Buy : SignalType::Sell;
    e.confidence = std::clamp(
        std::clamp(std::abs(e1 - e5) / atr, 0.0, 1.0) * conf_scale, 0.0, 1.0);
    e.reason = (Resonance::Bull == res)
        ? "EMA resonance bullish alignment (all 5 EMAs ordered)"
        : "EMA resonance bearish alignment (all 5 EMAs ordered)";
    return e;
}

} // namespace pulse::strategy
