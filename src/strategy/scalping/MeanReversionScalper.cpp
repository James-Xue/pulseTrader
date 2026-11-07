// mean_reversion_scalper.cpp — Bollinger Band mean-reversion (Layer 6 Strategy Engine)

#include "strategy/scalping/MeanReversionScalper.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>

namespace pulse::strategy
{

std::string MeanReversionScalper::className() const
{
    return "MeanReversionScalper";
}

std::string MeanReversionScalper::idPrefix() const
{
    return "mean_reversion_scalper";
}

std::size_t MeanReversionScalper::klineNeeded() const
{
    return static_cast<std::size_t>(
        m_params.bb_period.load(std::memory_order_acquire));
}

std::optional<EntryContext> MeanReversionScalper::evaluateEntry(
    const std::vector<market::Kline> &candles)
{
    // 1. Read hot-reloadable parameters.
    const double bb_std_dev = m_params.bb_std_dev.load(std::memory_order_acquire);

    // 2. Extract close prices.
    std::vector<double> closes;
    closes.reserve(candles.size());
    for (const auto &c : candles)
    {
        closes.push_back(c.close);
    }

    // 3. Compute SMA (simple moving average).
    const double sum = std::accumulate(closes.begin(), closes.end(), 0.0);
    const double sma = sum / static_cast<double>(closes.size());

    // 4. Compute standard deviation.
    double sq_sum = 0.0;
    for (const double price : closes)
    {
        const double diff = price - sma;
        sq_sum += diff * diff;
    }
    const double stddev = std::sqrt(sq_sum / static_cast<double>(closes.size()));

    // 5. Compute Bollinger Bands.
    const double upper_band = sma + bb_std_dev * stddev;
    const double lower_band = sma - bb_std_dev * stddev;
    const double band_width = upper_band - lower_band;

    // 6. Get latest price (most recent close).
    const double current_price = closes.back();

    // 7. Check for band breach.
    const bool oversold = current_price <= lower_band;
    const bool overbought = current_price >= upper_band;

    if (!oversold && !overbought)
    {
        return std::nullopt; // Price is within bands — no signal.
    }

    // 8. Compute confidence: band penetration normalized by ATR14.
    //    ATR normalization keeps confidence meaningful across price scales
    //    (band-width ratio required ~60% of a 2σ band for 0.6 — near-unreachable
    //    on 1m gold, so signals were silently dropped by min_confidence).
    //    Falls back to the band-width ratio when ATR is unavailable.
    const double atr = computeAtr(candles, 14);
    const double penetration = oversold
        ? (lower_band - current_price)
        : (current_price - upper_band);
    const double confidence = computeConfidence(penetration, atr, band_width);

    EntryContext e;
    e.type = oversold ? SignalType::Buy : SignalType::Sell;
    e.price = current_price;
    e.confidence = confidence;
    e.atr = atr;
    e.reason = oversold
        ? "Price at/below lower Bollinger Band (oversold, mean reversion expected)"
        : "Price at/above upper Bollinger Band (overbought, mean reversion expected)";
    e.indicators = {
        { "bb_upper", upper_band },
        { "bb_lower", lower_band },
        { "bb_mid", sma },
        { "atr", atr },
    };
    return e;
}

void MeanReversionScalper::logSignal(const TradingSignal &sig) const
{
    PULSE_LOG_INFO("strategy",
        "[{}] {} signal: price={:.2f}, upper={:.2f}, lower={:.2f}, sma={:.2f}",
        id(), sig.reason, sig.price,
        sig.indicators.value("bb_upper", 0.0),
        sig.indicators.value("bb_lower", 0.0),
        sig.indicators.value("bb_mid", 0.0));
}

double MeanReversionScalper::computeConfidence(const double penetration,
    const double atr,
    const double band_width)
{
    if (atr > 0.0)
    {
        return std::clamp(penetration / atr, 0.0, 1.0);
    }
    if (band_width > 0.0)
    {
        return std::clamp(penetration / band_width, 0.0, 1.0);
    }
    return 0.0;
}

} // namespace pulse::strategy
