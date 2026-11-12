#pragma once

#include <QList>
#include <QtGlobal>

namespace pulse::client
{

/// One aggregated candlestick.
///
/// The aggregator receives one price sample per poll (the Market page polls at
/// 1 Hz) and folds every `periodMs / pollIntervalMs` samples into a single
/// candle. A candle is "closed" once the first sample of the next bucket
/// arrives; the newest candle is still forming and its close/high/low move on
/// every sample.
struct Candle
{
    qint64 bucket_start_ms = 0; ///< Wall-clock start of the bucket.
    double open = 0.0;          ///< First sample in the bucket.
    double high = 0.0;          ///< Highest sample in the bucket.
    double low = 0.0;           ///< Lowest sample in the bucket.
    double close = 0.0;         ///< Most recent sample in the bucket.
    int samples = 0;            ///< Number of samples folded into the candle.
    bool closed = false;        ///< True once the next bucket has started.
};

/// Folds a chronological stream of 1-second price samples into fixed-period
/// candles.
///
/// Design notes:
/// 1. Buckets are aligned to wall-clock multiples of `periodMs`, so a 10 s
///    candle always starts at :00, :10, :20, ... regardless of when streaming
///    began. Consumers can therefore label the x-axis with real timestamps.
/// 2. A gap between samples never synthesises flat candles. If the engine was
///    unreachable for a while, the next sample simply starts a later bucket;
///    the caller can see the gap from `bucket_start_ms`.
/// 3. Out-of-order samples (a wall clock that jumped backwards) are ignored;
///    the newest candle must remain the last element of the list.
/// 4. Non-finite and non-positive prices are rejected. A zero price is never a
///    valid trade and would otherwise flatten the y-axis to a single point.
///
/// Thread affinity: GUI thread only. No locking is needed.
class CandleAggregator
{
  public:
    /// Create an empty aggregator.
    ///
    /// Parameters:
    ///   1. periodMs    — candle period in milliseconds (default 10 s).
    ///   2. maxCandles  — ring capacity; the oldest candle is dropped first.
    explicit CandleAggregator(qint64 periodMs = 10'000, int maxCandles = 120);

    /// Fold one price sample into the current candle.
    ///
    /// Returns true when the sample was accepted. A sample is rejected when
    /// the price is non-finite/non-positive, the timestamp is negative, or the
    /// sample belongs to an older bucket than the current one.
    bool addSample(double price, qint64 timestampMs);

    /// Change the candle period. Any existing candles are discarded because
    /// their buckets no longer line up with the new period.
    void setPeriodMs(qint64 periodMs);

    /// Drop every candle. The period is preserved.
    void clear();

    [[nodiscard]] qint64 periodMs() const
    {
        return m_periodMs;
    }

    [[nodiscard]] int capacity() const
    {
        return m_maxCandles;
    }

    /// Oldest-to-newest candles. The last element is the forming candle.
    [[nodiscard]] const QList<Candle>& candles() const
    {
        return m_candles;
    }

  private:
    qint64 m_periodMs;
    int m_maxCandles;
    QList<Candle> m_candles;
};

} // namespace pulse::client