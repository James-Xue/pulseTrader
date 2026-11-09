#include "data/IndicatorEngine.hpp"

#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pulse::client::data
{

namespace
{

/// "Undefined" marker used inside the numeric series. NaN propagates through
/// the recursions instead of masquerading as a real zero.
constexpr double kUndefined = std::numeric_limits<double>::quiet_NaN();

/// True when the value is a usable number. std::isfinite also rejects NaN and
/// the infinities, so one call covers every escape route.
bool isDefined(double value)
{
    return std::isfinite(value);
}

/// Wrap a double for JSON, turning an undefined value into null.
QJsonValue jsonOrNull(double value)
{
    return isDefined(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

/// Last defined entry of a series, or NaN when the series never becomes
/// defined. Callers only publish the most recent value, so this is the only
/// element they need.
double lastDefined(const QVector<double>& series)
{
    for (int i = series.size() - 1; i >= 0; --i)
    {
        if (isDefined(series[i]))
        {
            return series[i];
        }
    }
    return kUndefined;
}

/// SuperTrend output for the newest bar.
struct SuperTrendPoint
{
    double value = kUndefined; ///< Band the trend is riding, NaN if undefined.
    int direction = 0;         ///< +1 up, -1 down, 0 undefined.
};

/// SuperTrend with Wilder-smoothed ATR.
///
/// Algorithm (matching the canonical Pine/TradingView formulation):
/// 1. True range is max(high - low, |high - previous close|,
///    |low - previous close|).
/// 2. The first ATR value is the SMA of the first `atrPeriod` true ranges
///    (indexes 1..atrPeriod); afterwards the recursion is
///    ATR[i] = (ATR[i-1] * (n - 1) + TR[i]) / n.
/// 3. Basic bands are hl2 +/- multiplier * ATR.
/// 4. The final upper band only ratchets down while price stays below it; the
///    final lower band only ratchets up while price stays above it.
/// 5. The trend starts in the "up" state on the first bar that has an ATR and
///    flips to "down" when the close drops below the previous final upper
///    band, or to "up" when it rises above the previous final lower band.
///    (This mirrors Pine's `nz(trend[1], 1)` initialisation.)
SuperTrendPoint superTrendPoint(const QVector<double>& highs,
                                const QVector<double>& lows,
                                const QVector<double>& closes,
                                int atrPeriod,
                                double multiplier)
{
    SuperTrendPoint point;

    const int count = closes.size();
    if (atrPeriod < 1 || count < atrPeriod + 1)
    {
        return point;
    }

    // 1. True range per bar. Index 0 has no previous close, so it is the plain
    //    high-low range; the SMA below deliberately starts at index 1.
    QVector<double> trueRange(count, kUndefined);
    trueRange[0] = highs[0] - lows[0];
    for (int i = 1; i < count; ++i)
    {
        const double previousClose = closes[i - 1];
        trueRange[i] = std::max({ highs[i] - lows[i],
                                  std::fabs(highs[i] - previousClose),
                                  std::fabs(lows[i] - previousClose) });
    }

    // 2. Seed the ATR at the first index where `atrPeriod` true ranges exist.
    double atr = 0.0;
    for (int i = 1; i <= atrPeriod; ++i)
    {
        atr += trueRange[i];
    }
    atr /= atrPeriod;

    double finalUpper = kUndefined;
    double finalLower = kUndefined;
    int direction = 0;
    double previousClose = closes[0];

    for (int i = 1; i < count; ++i)
    {
        // 2b. Wilder smoothing for every bar past the seed.
        if (i > atrPeriod)
        {
            atr = ((atr * (atrPeriod - 1)) + trueRange[i]) / atrPeriod;
        }

        if (i >= atrPeriod)
        {
            const double mid = (highs[i] + lows[i]) / 2.0;
            const double basicUpper = mid + (multiplier * atr);
            const double basicLower = mid - (multiplier * atr);

            if (0 == direction)
            {
                // 5a. First computable bar: both bands start at their basic
                //     values and the trend is decided by the same-bar close.
                finalUpper = basicUpper;
                finalLower = basicLower;
                direction = (closes[i] < basicUpper) ? -1 : 1;
            }
            else
            {
                // 4. Ratchet the bands; a close beyond a band resets it to the
                //    basic value, which is what lets the trend flip later.
                const double previousUpper = finalUpper;
                const double previousLower = finalLower;

                finalUpper =
                    (basicUpper < previousUpper || previousClose > previousUpper)
                        ? basicUpper
                        : previousUpper;
                finalLower =
                    (basicLower > previousLower || previousClose < previousLower)
                        ? basicLower
                        : previousLower;

                // 5b. Flip on a close beyond the previous bar's band.
                if (direction > 0 && closes[i] < previousUpper)
                {
                    direction = -1;
                }
                else if (direction < 0 && closes[i] > previousLower)
                {
                    direction = 1;
                }
            }
        }

        previousClose = closes[i];
    }

    if (0 != direction)
    {
        // The trend line rides the protective band: the lower band in an
        // uptrend, the upper band in a downtrend.
        point.value = (direction > 0) ? finalLower : finalUpper;
        point.direction = direction;
    }
    return point;
}

} // namespace

IndicatorEngine::IndicatorEngine()
    : m_params()
{
}

IndicatorEngine::IndicatorEngine(Params params)
    : m_params(std::move(params))
{
}

QVector<double> IndicatorEngine::emaSeries(const QVector<double>& values, int period)
{
    QVector<double> series(values.size(), kUndefined);
    if (period < 1 || values.size() < period)
    {
        return series;
    }

    // 1. Seed with the simple average of the first `period` values. Seeding
    //    with the first value instead would bias every later value.
    double sum = 0.0;
    for (int i = 0; i < period; ++i)
    {
        sum += values[i];
    }
    series[period - 1] = sum / period;

    // 2. Standard exponential smoothing from the seed onwards.
    const double smoothing = 2.0 / (period + 1.0);
    for (int i = period; i < values.size(); ++i)
    {
        series[i] = series[i - 1] + (smoothing * (values[i] - series[i - 1]));
    }
    return series;
}

QJsonObject IndicatorEngine::compute(const QJsonArray& klines) const
{
    // 1. Keep only finalized candles. The engine appends the live candle with
    //    closed = false; including it would move every indicator on each tick.
    QVector<double> closes;
    QVector<double> highs;
    QVector<double> lows;
    closes.reserve(klines.size());
    highs.reserve(klines.size());
    lows.reserve(klines.size());

    for (const QJsonValue& entry : klines)
    {
        const QJsonObject kline = entry.toObject();
        if (!kline.value(QStringLiteral("closed")).toBool())
        {
            continue;
        }
        closes.append(kline.value(QStringLiteral("close")).toDouble());
        highs.append(kline.value(QStringLiteral("high")).toDouble());
        lows.append(kline.value(QStringLiteral("low")).toDouble());
    }

    QJsonObject indicators;

    // 2. EMA block: one key per configured period, keyed by the period itself
    //    so the consumer can look values up without a fixed schema.
    QJsonObject ema;
    for (int period : m_params.emaPeriods)
    {
        const QVector<double> series = emaSeries(closes, period);
        ema[QString::number(period)] = jsonOrNull(lastDefined(series));
    }
    indicators[QStringLiteral("ema")] = ema;

    // 3. MACD: line = EMA(fast) - EMA(slow); signal = EMA of the defined part
    //    of the line; histogram = line - signal. The signal is computed over
    //    the compacted line so its own SMA seed starts at the first bar where
    //    the line exists, not at bar zero.
    const QVector<double> fast = emaSeries(closes, m_params.macdFast);
    const QVector<double> slow = emaSeries(closes, m_params.macdSlow);
    QVector<double> line(fast.size(), kUndefined);
    QVector<double> definedLine;
    for (int i = 0; i < fast.size(); ++i)
    {
        if (isDefined(fast[i]) && isDefined(slow[i]))
        {
            line[i] = fast[i] - slow[i];
            definedLine.append(line[i]);
        }
    }
    const QVector<double> signalSeries =
        emaSeries(definedLine, m_params.macdSignal);

    const double lineValue = lastDefined(line);
    const double signalValue = lastDefined(signalSeries);
    const double histogram =
        (isDefined(lineValue) && isDefined(signalValue))
            ? lineValue - signalValue
            : kUndefined;

    QJsonObject macd;
    macd[QStringLiteral("line")] = jsonOrNull(lineValue);
    macd[QStringLiteral("signal")] = jsonOrNull(signalValue);
    macd[QStringLiteral("hist")] = jsonOrNull(histogram);
    indicators[QStringLiteral("macd")] = macd;

    // 4. SuperTrend block. The direction is only meaningful together with a
    //    value, so both are null until enough bars have accumulated.
    const SuperTrendPoint trend = superTrendPoint(
        highs, lows, closes, m_params.atrPeriod, m_params.supertrendMultiplier);

    QJsonObject supertrend;
    supertrend[QStringLiteral("value")] = jsonOrNull(trend.value);
    supertrend[QStringLiteral("direction")] =
        (0 == trend.direction)
            ? QJsonValue(QJsonValue::Null)
            : QJsonValue(0 > trend.direction ? QStringLiteral("down")
                                             : QStringLiteral("up"));
    indicators[QStringLiteral("supertrend")] = supertrend;

    return indicators;
}

} // namespace pulse::client::data
