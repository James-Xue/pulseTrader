#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>

#include "data/IndicatorEngine.hpp"

using pulse::client::data::IndicatorEngine;

/// Known-vector tests for the indicator math.
///
/// The vectors are chosen so the expected value can be derived on paper:
/// a linear ramp makes the EMA lag exactly (period - 1) / 2, which pins the
/// MACD spread, and a four-bar SuperTrend vector is small enough to walk the
/// ATR and band recursion by hand.
class TestIndicators : public QObject
{
    Q_OBJECT

private slots:
    void emaSeedsWithSma();
    void emaUndefinedBeforeSeed();
    void macdOfLinearRamp();
    void macdNeedsLongHistory();
    void supertrendHandComputed();
    void supertrendNeedsEnoughBars();
    void unclosedKlinesAreIgnored();
    void everyConfiguredEmaPeriodIsReported();
    void emptyInputYieldsNulls();

private:
    static QJsonObject kline(double high, double low, double close, bool closed)
    {
        QJsonObject object;
        object[QStringLiteral("open_time")] = 0;
        object[QStringLiteral("close_time")] = 0;
        object[QStringLiteral("open")] = close;
        object[QStringLiteral("high")] = high;
        object[QStringLiteral("low")] = low;
        object[QStringLiteral("close")] = close;
        object[QStringLiteral("volume")] = 0.0;
        object[QStringLiteral("closed")] = closed;
        return object;
    }

    /// Ramp of `count` closed candles whose close is `first + i`.
    static QJsonArray ramp(int count, double first)
    {
        QJsonArray klines;
        for (int i = 0; i < count; ++i)
        {
            const double value = first + i;
            klines.append(kline(value + 0.5, value - 0.5, value, true));
        }
        return klines;
    }

    /// A kline's numeric field as a double, or NaN when it is JSON null.
    static double number(const QJsonObject& object, const QString& key)
    {
        const QJsonValue value = object.value(key);
        return value.isDouble() ? value.toDouble() : std::nan("");
    }
};

void TestIndicators::emaSeedsWithSma()
{
    QVector<double> values;
    for (int i = 1; i <= 20; ++i)
    {
        values.append(i);
    }

    // 1. The first defined entry is the SMA of the first five values: 3.0.
    //    A first-value seed would have produced 1.0 here.
    const QVector<double> series = IndicatorEngine::emaSeries(values, 5);
    QCOMPARE(series.size(), values.size());
    QVERIFY(std::abs(series[4] - 3.0) < 1e-12);

    // 2. On a linear ramp the EMA lags the input by (period - 1) / 2 forever,
    //    so the last value (20) must land on 18.0.
    QVERIFY(std::abs(series.constLast() - 18.0) < 1e-9);
}

void TestIndicators::emaUndefinedBeforeSeed()
{
    QVector<double> values{ 1.0, 2.0, 3.0, 4.0, 5.0 };

    const QVector<double> series = IndicatorEngine::emaSeries(values, 5);

    // Indexes 0..3 have not accumulated a full window yet.
    for (int i = 0; i < 4; ++i)
    {
        QVERIFY(qIsNaN(series[i]));
    }
    QVERIFY(std::isfinite(series[4]));
}

void TestIndicators::macdOfLinearRamp()
{
    const IndicatorEngine engine;
    const QJsonObject indicators = engine.compute(ramp(100, 1.0));
    const QJsonObject macd = indicators.value(QStringLiteral("macd")).toObject();

    // EMA12 lags by 5.5 and EMA26 by 12.5, so the MACD line is a constant 7.0
    // and its EMA9 signal is 7.0 as well. The histogram must be zero.
    QVERIFY(std::abs(number(macd, QStringLiteral("line")) - 7.0) < 1e-9);
    QVERIFY(std::abs(number(macd, QStringLiteral("signal")) - 7.0) < 1e-9);
    QVERIFY(std::abs(number(macd, QStringLiteral("hist"))) < 1e-9);
}

void TestIndicators::macdNeedsLongHistory()
{
    const IndicatorEngine engine;

    // 20 bars cannot seed EMA26 (index 25), so the line is undefined.
    QJsonObject macd = engine.compute(ramp(20, 1.0))
                           .value(QStringLiteral("macd"))
                           .toObject();
    QVERIFY(macd.value(QStringLiteral("line")).isNull());
    QVERIFY(macd.value(QStringLiteral("signal")).isNull());
    QVERIFY(macd.value(QStringLiteral("hist")).isNull());
}

void TestIndicators::supertrendHandComputed()
{
    // ATR(2) with a multiplier of 1 keeps every number small enough to verify
    // by hand. Walk with closes 10, 11, 12, 13 and a one-wide high/low band:
    //   TR = 2 for bars 1..3, so ATR = 2 at bar 2.
    //   Bar 2: hl2 = 12, upper = 14, lower = 10; close 12 < 14, trend = down,
    //          SuperTrend rides the upper band at 14.
    //   Bar 3: hl2 = 13, basic upper = 15, basic lower = 11. The upper band
    //          ratchets to max(14, ...) = 14 because the previous close stayed
    //          below it; the lower band ratchets up to 11. Close 13 > 10 (the
    //          previous lower band), so the trend flips up and the line sits
    //          on the lower band at 11.
    IndicatorEngine::Params params;
    params.atrPeriod = 2;
    params.supertrendMultiplier = 1.0;

    QJsonArray klines;
    klines.append(kline(11.0, 9.0, 10.0, true));
    klines.append(kline(12.0, 10.0, 11.0, true));
    klines.append(kline(13.0, 11.0, 12.0, true));
    klines.append(kline(14.0, 12.0, 13.0, true));

    const QJsonObject supertrend =
        IndicatorEngine(params)
            .compute(klines)
            .value(QStringLiteral("supertrend"))
            .toObject();

    QVERIFY(std::abs(number(supertrend, QStringLiteral("value")) - 11.0) < 1e-9);
    QCOMPARE(supertrend.value(QStringLiteral("direction")).toString(),
             QStringLiteral("up"));
}

void TestIndicators::supertrendNeedsEnoughBars()
{
    IndicatorEngine::Params params;
    params.atrPeriod = 2;
    params.supertrendMultiplier = 1.0;

    QJsonArray klines;
    klines.append(kline(11.0, 9.0, 10.0, true));
    klines.append(kline(12.0, 10.0, 11.0, true));

    const QJsonObject supertrend =
        IndicatorEngine(params)
            .compute(klines)
            .value(QStringLiteral("supertrend"))
            .toObject();

    // ATR needs atrPeriod true ranges, i.e. atrPeriod + 1 candles.
    QVERIFY(supertrend.value(QStringLiteral("value")).isNull());
    QVERIFY(supertrend.value(QStringLiteral("direction")).isNull());
}

void TestIndicators::unclosedKlinesAreIgnored()
{
    // A wild, still-open candle must not move any indicator: the values have
    // to match the same history without it.
    QJsonArray klines = ramp(100, 1.0);
    const QJsonObject baseline =
        IndicatorEngine{}.compute(klines).value(QStringLiteral("macd")).toObject();

    klines.append(kline(1.0e9, 0.0, 1.0e9, false));
    const QJsonObject withLive =
        IndicatorEngine{}.compute(klines).value(QStringLiteral("macd")).toObject();

    QVERIFY(std::abs(number(baseline, QStringLiteral("line"))
                     - number(withLive, QStringLiteral("line"))) < 1e-12);
    QVERIFY(std::abs(number(baseline, QStringLiteral("signal"))
                     - number(withLive, QStringLiteral("signal"))) < 1e-12);
}

void TestIndicators::everyConfiguredEmaPeriodIsReported()
{
    const QJsonObject ema =
        IndicatorEngine{}.compute(ramp(250, 1.0)).value(QStringLiteral("ema")).toObject();

    for (int period : { 7, 14, 30, 60, 200 })
    {
        QVERIFY2(ema.contains(QString::number(period)),
                 qPrintable(QStringLiteral("missing EMA %1").arg(period)));
        QVERIFY(ema.value(QString::number(period)).isDouble());
    }

    // The shortest EMA is always computable; the longest is too after 250 bars.
    QVERIFY(std::abs(number(ema, QStringLiteral("200")) - (250.0 - 99.5)) < 1e-6);
}

void TestIndicators::emptyInputYieldsNulls()
{
    const QJsonObject indicators = IndicatorEngine{}.compute(QJsonArray{});

    const QJsonObject ema = indicators.value(QStringLiteral("ema")).toObject();
    const QJsonObject macd = indicators.value(QStringLiteral("macd")).toObject();
    const QJsonObject supertrend =
        indicators.value(QStringLiteral("supertrend")).toObject();

    QVERIFY(ema.value(QStringLiteral("7")).isNull());
    QVERIFY(macd.value(QStringLiteral("line")).isNull());
    QVERIFY(supertrend.value(QStringLiteral("value")).isNull());
    QVERIFY(supertrend.value(QStringLiteral("direction")).isNull());
}

QTEST_MAIN(TestIndicators)
#include "tst_indicators.moc"
