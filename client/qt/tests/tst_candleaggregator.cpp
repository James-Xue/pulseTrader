#include <QTest>

#include <cmath>
#include <limits>

#include "market/CandleAggregator.hpp"

using pulse::client::Candle;
using pulse::client::CandleAggregator;

/// Pure arithmetic over synthetic samples: no sockets, no engine, no clock.
class TestCandleAggregator : public QObject
{
    Q_OBJECT

  private slots:
    void firstSampleOpensCandle();
    void sameBucketFoldsOhlc();
    void nextBucketClosesPrevious();
    void gapDoesNotSynthesiseCandles();
    void outOfOrderSampleIgnored();
    void invalidPriceRejected();
    void capacityDropsOldest();
    void periodChangeClears();
    void boundaryBelongsToNewBucket();
    void repeatedTimestampUpdates();
};

void TestCandleAggregator::firstSampleOpensCandle()
{
    CandleAggregator aggregator(10'000, 10);
    QVERIFY(aggregator.addSample(100.0, 1'000));

    QCOMPARE(aggregator.candles().size(), 1);
    const Candle& candle = aggregator.candles().first();
    QCOMPARE(candle.bucket_start_ms, qint64(0));
    QCOMPARE(candle.open, 100.0);
    QCOMPARE(candle.high, 100.0);
    QCOMPARE(candle.low, 100.0);
    QCOMPARE(candle.close, 100.0);
    QCOMPARE(candle.samples, 1);
    QVERIFY(!candle.closed);
}

void TestCandleAggregator::sameBucketFoldsOhlc()
{
    CandleAggregator aggregator(10'000, 10);
    QVERIFY(aggregator.addSample(100.0, 1'000));
    QVERIFY(aggregator.addSample(102.5, 2'000));
    QVERIFY(aggregator.addSample(99.25, 9'999));

    QCOMPARE(aggregator.candles().size(), 1);
    const Candle& candle = aggregator.candles().first();
    QCOMPARE(candle.open, 100.0);
    QCOMPARE(candle.high, 102.5);
    QCOMPARE(candle.low, 99.25);
    QCOMPARE(candle.close, 99.25);
    QCOMPARE(candle.samples, 3);
    QVERIFY(!candle.closed);
}

void TestCandleAggregator::nextBucketClosesPrevious()
{
    CandleAggregator aggregator(10'000, 10);
    QVERIFY(aggregator.addSample(100.0, 1'000));
    QVERIFY(aggregator.addSample(101.0, 10'000));

    QCOMPARE(aggregator.candles().size(), 2);
    QVERIFY(aggregator.candles().first().closed);
    QCOMPARE(aggregator.candles().first().close, 100.0);

    const Candle& fresh = aggregator.candles().last();
    QCOMPARE(fresh.bucket_start_ms, qint64(10'000));
    QCOMPARE(fresh.open, 101.0);
    QCOMPARE(fresh.high, 101.0);
    QCOMPARE(fresh.low, 101.0);
    QCOMPARE(fresh.close, 101.0);
    QCOMPARE(fresh.samples, 1);
    QVERIFY(!fresh.closed);
}

void TestCandleAggregator::gapDoesNotSynthesiseCandles()
{
    CandleAggregator aggregator(10'000, 10);
    QVERIFY(aggregator.addSample(100.0, 1'000));
    // A 30-second gap: the next sample lands in the bucket at 30 s. The
    // aggregator must not invent the two missing buckets.
    QVERIFY(aggregator.addSample(105.0, 31'000));

    QCOMPARE(aggregator.candles().size(), 2);
    QCOMPARE(aggregator.candles().last().bucket_start_ms, qint64(30'000));
    QCOMPARE(aggregator.candles().last().open, 105.0);
}

void TestCandleAggregator::outOfOrderSampleIgnored()
{
    CandleAggregator aggregator(10'000, 10);
    QVERIFY(aggregator.addSample(100.0, 11'000));
    QVERIFY(aggregator.addSample(105.0, 12'000));

    // A backwards wall-clock jump must not rewrite history.
    QVERIFY(!aggregator.addSample(50.0, 5'000));
    QCOMPARE(aggregator.candles().size(), 1);
    QCOMPARE(aggregator.candles().last().open, 100.0);
    QCOMPARE(aggregator.candles().last().close, 105.0);
    QCOMPARE(aggregator.candles().last().samples, 2);
}

void TestCandleAggregator::invalidPriceRejected()
{
    CandleAggregator aggregator(10'000, 10);

    QVERIFY(!aggregator.addSample(0.0, 1'000));
    QVERIFY(!aggregator.addSample(-1.0, 1'000));
    QVERIFY(!aggregator.addSample(std::numeric_limits<double>::quiet_NaN(), 1'000));
    QVERIFY(!aggregator.addSample(std::numeric_limits<double>::infinity(), 1'000));
    QVERIFY(!aggregator.addSample(100.0, -1));

    QVERIFY(aggregator.candles().isEmpty());
}

void TestCandleAggregator::capacityDropsOldest()
{
    CandleAggregator aggregator(1'000, 3);
    QVERIFY(aggregator.addSample(100.0, 1'000));
    QVERIFY(aggregator.addSample(101.0, 2'000));
    QVERIFY(aggregator.addSample(102.0, 3'000));
    QVERIFY(aggregator.addSample(103.0, 4'000));

    QCOMPARE(aggregator.candles().size(), 3);
    QCOMPARE(aggregator.candles().first().bucket_start_ms, qint64(2'000));
    QCOMPARE(aggregator.candles().last().bucket_start_ms, qint64(4'000));
}

void TestCandleAggregator::periodChangeClears()
{
    CandleAggregator aggregator(10'000, 10);
    QVERIFY(aggregator.addSample(100.0, 1'000));
    QCOMPARE(aggregator.candles().size(), 1);

    aggregator.setPeriodMs(5'000);
    QCOMPARE(aggregator.periodMs(), qint64(5'000));
    QVERIFY(aggregator.candles().isEmpty());

    QVERIFY(aggregator.addSample(101.0, 6'000));
    QCOMPARE(aggregator.candles().size(), 1);
    QCOMPARE(aggregator.candles().first().bucket_start_ms, qint64(5'000));
}

void TestCandleAggregator::boundaryBelongsToNewBucket()
{
    CandleAggregator aggregator(10'000, 10);
    QVERIFY(aggregator.addSample(100.0, 9'999));
    QVERIFY(aggregator.addSample(101.0, 10'000));

    QCOMPARE(aggregator.candles().size(), 2);
    QCOMPARE(aggregator.candles().first().bucket_start_ms, qint64(0));
    QCOMPARE(aggregator.candles().last().bucket_start_ms, qint64(10'000));
}

void TestCandleAggregator::repeatedTimestampUpdates()
{
    CandleAggregator aggregator(10'000, 10);
    QVERIFY(aggregator.addSample(100.0, 1'000));
    QVERIFY(aggregator.addSample(101.0, 1'000));
    QVERIFY(aggregator.addSample(99.0, 1'000));

    QCOMPARE(aggregator.candles().size(), 1);
    const Candle& candle = aggregator.candles().first();
    QCOMPARE(candle.samples, 3);
    QCOMPARE(candle.open, 100.0);
    QCOMPARE(candle.high, 101.0);
    QCOMPARE(candle.low, 99.0);
    QCOMPARE(candle.close, 99.0);
}

QTEST_MAIN(TestCandleAggregator)

#include "tst_candleaggregator.moc"