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
    const double spike_filter_pct = customParam("eth_spike_filter_pct", 1.5);
    const double spike_filter_atr = customParam("eth_spike_filter_atr", 3.0);
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

    // 4. Trend state (v2): published on EVERY candle (Flat, confidence 0)
    //    so the signal board always carries the current trend gate for the
    //    grid sub-agent — a persistent state, not just the cross event.
    std::string trend_state = "neutral";
    if (ema_fast < ema_slow)
    {
        trend_state = "bearish";
    }
    else if (ema_fast > ema_slow)
    {
        trend_state = "bullish";
    }

    // 5. ATR — needed for both the confidence normalization and the
    //    ATR-relative spike filter.
    const double atr = computeAtr(candles, 14);

    // 6. Spike filter (v2): ANY of the three thresholds tripping = spike
    //    (暴拉/插针, do NOT chase it short). The USD-only filter missed the
    //    04:50 1m +4.4% pump in the grid review — a percent or ATR-relative
    //    threshold catches fast pumps on any price scale. Set one to 0 to
    //    disable it (a threshold of 0 means "no filter", not "any range").
    const auto &latest = candles.back();
    const double range_usd = latest.high - latest.low;
    const double range_pct = latest.close > 0.0
                                 ? range_usd / latest.close * 100.0 : 0.0;
    const bool spike = (spike_filter_usd > 0.0 && range_usd > spike_filter_usd)
                       || (spike_filter_pct > 0.0 && range_pct > spike_filter_pct)
                       || (atr > 0.0 && spike_filter_atr > 0.0
                           && range_usd > atr * spike_filter_atr);

    // 7. Short-only entry: the bearish crossover fires the real Sell signal;
    //    bullish crossovers are ignored (chase-short regime).
    const bool bearish_cross = m_hasPrev
        && (m_prevEmaFast >= m_prevEmaSlow) && (ema_fast < ema_slow);

    EntryContext e;
    e.price = latest.close;
    e.atr = atr;
    e.indicators = {
        { "ema_fast", ema_fast },
        { "ema_slow", ema_slow },
        { "trend_state", trend_state },
        { "atr", atr },
        { "atr_step", atr_step },
        { "suggested_tp", latest.close - atr_step * atr },
        { "spike", spike ? 1 : 0 },
        { "spike_range_usd", range_usd },
        { "spike_range_pct", range_pct },
        { "spike_filter_usd", spike_filter_usd },
        { "spike_filter_pct", spike_filter_pct },
        { "spike_filter_atr", spike_filter_atr },
    };

    if (bearish_cross && !spike && atr > 0.0)
    {
        // 8. Confidence: ATR-normalized EMA separation, scaled by the
        //    coin-specific factor, clamped to [0, 1].
        double confidence = std::clamp(
            std::abs(ema_fast - ema_slow) / atr, 0.0, 1.0);
        confidence = std::clamp(confidence * conf_scale, 0.0, 1.0);

        e.type = SignalType::Sell;
        e.confidence = confidence;
        e.reason = "ETH short setup: EMA bearish crossover (fast < slow)";
    }
    else
    {
        // State-only publish: no trade signal, but the board keeps the
        // current trend gate / spike flag fresh for consumers.
        e.type = SignalType::Flat;
        e.confidence = 0.0;
        if (bearish_cross && spike)
        {
            e.reason = "ETH state: bearish crossover but spike filter tripped "
                       "(range > usd/pct/atr threshold) — no chase";
        }
        else
        {
            e.reason = "ETH state: EMA trend " + trend_state
                       + (spike ? " with spike" : "") + " — no signal";
        }
    }

    // 9. Store current EMAs for next crossover detection (committed even
    //    when no signal fired — the trend has moved on regardless).
    m_prevEmaFast = ema_fast;
    m_prevEmaSlow = ema_slow;
    m_hasPrev = true;
    return e;
}

} // namespace pulse::strategy
