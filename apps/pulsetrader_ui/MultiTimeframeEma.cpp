// multi_timeframe_ema.cpp — implementation of the EMA reference panel.

#include "MultiTimeframeEma.hpp"

#include "EngineStrategyAdapter.hpp"
#include "VerifyWorker.hpp"

#include "exchange/GateRestClient.hpp"

#include "market/KlineBuffer.hpp"  // pulse::market::Kline (POD struct)

#include <algorithm>

namespace pulse::ui
{

namespace
{

// Candle history cap per timeframe.
constexpr std::size_t kMaxCandles = 500;
// EMA stack warm-up: the slowest EMA (180) needs 180 closes for its SMA seed.
constexpr std::size_t kMinCandles = 180;

// Gate TradFi kline_type per timeframe label; "60m" maps to "1h".
QString klineTypeFor(const QString &label)
{
    return ("60m" == label) ? QStringLiteral("1h") : label;
}

} // namespace

const std::vector<int> &MultiTimeframeEma::periods()
{
    static const std::vector<int> kPeriods = { 7, 14, 31, 60, 90, 120, 180 };
    return kPeriods;
}

const std::vector<QString> &MultiTimeframeEma::timeframes()
{
    static const std::vector<QString> kTimeframes = {
        QStringLiteral("1m"), QStringLiteral("3m"), QStringLiteral("5m"),
        QStringLiteral("10m"), QStringLiteral("15m"), QStringLiteral("30m"),
        QStringLiteral("60m"),
    };
    return kTimeframes;
}

MultiTimeframeEma::~MultiTimeframeEma() = default;

MultiTimeframeEma::MultiTimeframeEma()
{
    const auto &labels = timeframes();
    const std::int64_t minutes[] = { 1, 3, 5, 10, 15, 30, 60 };
    m_tracks.reserve(labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        Track track;
        track.periodMs = minutes[i] * 60'000;
        track.label = labels[i];
        m_tracks.push_back(std::move(track));
    }
}

int MultiTimeframeEma::warmup(pulse::exchange::GateRestClient &cfd)
{
    int total = 0;
    for (auto &track : m_tracks)
    {
        const auto klines = cfd.getCfdKlines(
            "XAUUSD", 500, klineTypeFor(track.label).toStdString());
        if (false == pulse::ok(klines))
        {
            continue;  // Unsupported kline_type or network — organic warm-up.
        }
        const nlohmann::json *list = nullptr;
        const nlohmann::json *obj = pulse::ui::VerifyWorker::unwrapData(
            pulse::ui::VerifyWorker::firstObject(pulse::value(klines)));
        if (nullptr != obj && obj->contains("list") && (*obj)["list"].is_array())
        {
            list = &(*obj)["list"];
        }
        else if (pulse::value(klines).is_array())
        {
            list = &pulse::value(klines);
        }
        if (nullptr == list)
        {
            continue;
        }
        track.klines.clear();
        for (const auto &item : *list)
        {
            pulse::market::Kline kline;
            if (EngineStrategyAdapter::parseKline(item, kline))
            {
                track.klines.push_back(kline);
            }
        }
        total += static_cast<int>(track.klines.size());
        track.hasBackfill = (false == track.klines.empty());
    }
    return total;
}

void MultiTimeframeEma::onTick(double price, std::int64_t nowMs)
{
    for (auto &track : m_tracks)
    {
        const std::int64_t bucket = nowMs / track.periodMs * track.periodMs;
        if (track.currentBucketMs < 0 || bucket != track.currentBucketMs)
        {
            if (false == track.klines.empty() && false == track.klines.back().closed)
            {
                track.klines.back().closed = true;
            }
            pulse::market::Kline fresh;
            fresh.open_time = bucket;
            fresh.close_time = bucket + track.periodMs;
            fresh.open = price;
            fresh.high = price;
            fresh.low = price;
            fresh.close = price;
            fresh.closed = false;
            track.klines.push_back(fresh);
            if (track.klines.size() > kMaxCandles)
            {
                track.klines.erase(track.klines.begin());
            }
            track.currentBucketMs = bucket;
        }
        else
        {
            auto &cur = track.klines.back();
            cur.high = std::max(cur.high, price);
            cur.low = std::min(cur.low, price);
            cur.close = price;
        }
    }
}

std::vector<EmaRow> MultiTimeframeEma::snapshot() const
{
    std::vector<EmaRow> rows;
    rows.reserve(m_tracks.size());
    const auto &kPeriods = periods();
    for (const auto &track : m_tracks)
    {
        EmaRow row;
        row.period = track.label;
        row.available = track.hasBackfill;
        row.emas.assign(kPeriods.size(), 0.0);
        if (track.klines.size() < kMinCandles)
        {
            rows.push_back(row);  // valid = false → warming up.
            continue;
        }
        row.valid = true;
        // Standard EMA: SMA-seed on the FIRST `period` closes, then forward
        // recursion oldest → newest (the newest price dominates the result;
        // a monotonic stack on a trending series).
        for (std::size_t p = 0; p < kPeriods.size(); ++p)
        {
            const int period = kPeriods[p];
            if (static_cast<std::size_t>(period) > track.klines.size())
            {
                continue;
            }
            double sum = 0.0;
            for (std::size_t i = 0; i < static_cast<std::size_t>(period); ++i)
            {
                sum += track.klines[i].close;
            }
            const double k = 2.0 / (period + 1.0);
            double value = sum / period;
            for (std::size_t i = static_cast<std::size_t>(period);
                 i < track.klines.size(); ++i)
            {
                value = k * track.klines[i].close + (1.0 - k) * value;
            }
            row.emas[p] = value;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

int MultiTimeframeEma::candleCount(int timeframeIndex) const
{
    if (timeframeIndex < 0 || static_cast<std::size_t>(timeframeIndex) >= m_tracks.size())
    {
        return 0;
    }
    return static_cast<int>(m_tracks[timeframeIndex].klines.size());
}

} // namespace pulse::ui
