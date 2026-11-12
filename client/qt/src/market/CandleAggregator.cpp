#include "market/CandleAggregator.hpp"

#include <algorithm>
#include <cmath>

namespace pulse::client
{

CandleAggregator::CandleAggregator(qint64 periodMs, int maxCandles)
    : m_periodMs(periodMs > 0 ? periodMs : 10'000)
    , m_maxCandles(maxCandles > 0 ? maxCandles : 1)
{
}

bool CandleAggregator::addSample(double price, qint64 timestampMs)
{
    // 1. Reject values that cannot be plotted. The engine reports an empty
    //    ticker as 0.0, and NaN/Inf would poison the y-axis range downstream.
    if (!std::isfinite(price) || 0.0 >= price || 0 > timestampMs)
    {
        return false;
    }

    // 2. Align to the wall-clock grid. Integer division truncates toward zero,
    //    which is the desired floor for the non-negative epoch timestamps this
    //    class accepts.
    const qint64 bucketMs = (timestampMs / m_periodMs) * m_periodMs;

    // 3. First sample ever, or the first sample of a later bucket: close the
    //    previous candle and open a new one. A gap of several periods is kept
    //    as a gap rather than filled with synthetic flat candles.
    if (m_candles.isEmpty() || bucketMs > m_candles.last().bucket_start_ms)
    {
        if (!m_candles.isEmpty())
        {
            m_candles.last().closed = true;
        }

        Candle candle;
        candle.bucket_start_ms = bucketMs;
        candle.open = price;
        candle.high = price;
        candle.low = price;
        candle.close = price;
        candle.samples = 1;
        candle.closed = false;
        m_candles.append(candle);

        // Capacity is a hard bound: the chart only renders a window of the
        // newest candles, so dropping the oldest keeps memory flat no matter
        // how long the page streams.
        while (m_candles.size() > m_maxCandles)
        {
            m_candles.removeFirst();
        }
        return true;
    }

    // 4. Same bucket: fold the sample into the forming candle. The open is
    //    immutable, the close always follows the newest sample.
    if (bucketMs == m_candles.last().bucket_start_ms)
    {
        Candle& candle = m_candles.last();
        candle.high = std::max(candle.high, price);
        candle.low = std::min(candle.low, price);
        candle.close = price;
        ++candle.samples;
        return true;
    }

    // 5. Older bucket: a backwards wall-clock jump. Ignoring the sample keeps
    //    the list sorted, which every consumer relies on.
    return false;
}

void CandleAggregator::setPeriodMs(qint64 periodMs)
{
    if (0 >= periodMs || periodMs == m_periodMs)
    {
        return;
    }
    m_periodMs = periodMs;
    clear();
}

void CandleAggregator::clear()
{
    m_candles.clear();
}

} // namespace pulse::client