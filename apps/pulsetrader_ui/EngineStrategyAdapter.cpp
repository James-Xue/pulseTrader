// engine_strategy_adapter.cpp — implementation of the multi-timeframe
// SuperTrend adapter.

#include "EngineStrategyAdapter.hpp"

#include "VerifyWorker.hpp"

#include "strategy/scalping/SuperTrendScalper.hpp"

#include "exchange/GateRestClient.hpp"

#include "market/KlineBuffer.hpp"  // pulse::market::Kline (POD struct)

#include <algorithm>
#include <cstdio>
#include <exception>

namespace pulse::ui
{

namespace
{

// Candle history cap per timeframe (ATR period 10 + generous margin).
constexpr std::size_t kMaxCandles = 500;
// SuperTrend needs period + 1 candles; period defaults to 10.
constexpr std::size_t kMinCandles = 11;

// Numeric field reader: accepts a string or number; missing/wrong types
// yield nullopt.
std::optional<double> jsonNumber(const nlohmann::json &obj, const char *key)
{
    if (false == obj.is_object() || false == obj.contains(key))
    {
        return std::nullopt;
    }
    const auto &v = obj[key];
    if (v.is_string())
    {
        try
        {
            return std::stod(v.get<std::string>());
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }
    if (v.is_number())
    {
        return v.get<double>();
    }
    return std::nullopt;
}

} // namespace

bool EngineStrategyAdapter::parseKline(const nlohmann::json &obj, pulse::market::Kline &out)
{
    if (false == obj.is_object())
    {
        return false;
    }
    // Gate TradFi klines use the compact schema {o,h,l,c,t} (t in Unix
    // SECONDS). Full-name fields are accepted as a fallback.
    auto read = [&obj](const char *compact, const char *full) {
        auto v = jsonNumber(obj, compact);
        if (false == v.has_value())
        {
            v = jsonNumber(obj, full);
        }
        return v;
    };
    const auto open = read("o", "open");
    const auto high = read("h", "high");
    const auto low = read("l", "low");
    const auto close = read("c", "close");
    if (false == open.has_value() || false == high.has_value()
        || false == low.has_value() || false == close.has_value())
    {
        return false;
    }
    out.open = *open;
    out.high = *high;
    out.low = *low;
    out.close = *close;
    out.volume = jsonNumber(obj, "v").value_or(jsonNumber(obj, "volume").value_or(0.0));

    // Timestamp: "t" (seconds), "timestamp" (seconds or ms), "open_time" (ms).
    std::int64_t tMs = 0;
    auto t = jsonNumber(obj, "t");
    if (false == t.has_value())
    {
        t = jsonNumber(obj, "timestamp");
    }
    if (t.has_value())
    {
        tMs = *t < 1'000'000'000'000LL ? static_cast<std::int64_t>(*t * 1000.0)
                                       : static_cast<std::int64_t>(*t);
    }
    else
    {
        const auto openTime = jsonNumber(obj, "open_time");
        if (openTime.has_value())
        {
            tMs = static_cast<std::int64_t>(*openTime);
        }
    }
    out.open_time = tMs;
    out.close_time = tMs + 60'000;  // Placeholder; the aggregation fixes it.
    out.closed = true;
    return tMs > 0;
}

EngineStrategyAdapter::EngineStrategyAdapter()
{
    // Four timeframes in a fixed order (10s · 1m · 15m · 30m); each gets its
    // own SuperTrend instance with its own rolling band state. The strategy
    // never touches the injected context in evaluateState — a default
    // (all-null) context is safe.
    struct Spec
    {
        std::int64_t periodMs;
        const char *label;
    };
    const Spec specs[] = {
        { 10'000, "10s" }, { 60'000, "1m" }, { 900'000, "15m" }, { 1'800'000, "30m" },
    };
    m_tracks.reserve(4);
    for (const auto &spec : specs)
    {
        Track track;
        track.periodMs = spec.periodMs;
        track.label = QLatin1String(spec.label);
        track.strategy = std::make_unique<pulse::strategy::SuperTrendScalper>(
            pulse::strategy::StrategyContext{});
        m_tracks.push_back(std::move(track));
    }
}

EngineStrategyAdapter::~EngineStrategyAdapter() = default;

int EngineStrategyAdapter::warmup(pulse::exchange::GateRestClient &cfd)
{
    int total = 0;
    for (auto &track : m_tracks)
    {
        const auto klines = cfd.getCfdKlines("XAUUSD", 500, track.label.toStdString());
        if (false == pulse::ok(klines))
        {
            continue;  // This timeframe warms up organically from live polls.
        }
        // TradFi klines may come as {"data": {"list": [...]}} or a plain array.
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
            if (parseKline(item, kline))
            {
                track.klines.push_back(kline);
            }
        }
        total += static_cast<int>(track.klines.size());
    }
    return total;
}

TickOutcome EngineStrategyAdapter::onTick(double price, std::int64_t nowMs,
                                          double tpPoints, double slPoints,
                                          std::int64_t cooldownMs)
{
    TickOutcome outcome;
    outcome.trends.reserve(m_tracks.size());
    bool anyChanged = false;

    for (auto &track : m_tracks)
    {
        // 1. Aggregate the tick into this timeframe's candle.
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

        // 2. Evaluate the SuperTrend state (commits rolling state).
        TrendInfo trend;
        trend.label = track.label;
        if (track.klines.size() < kMinCandles)
        {
            outcome.trends.push_back(trend);  // valid = false → warming up.
            continue;
        }
        const auto state = track.strategy->evaluateState(track.klines);
        if (false == state.has_value())
        {
            outcome.trends.push_back(trend);
            continue;
        }
        trend.supertrend = state->value;
        trend.bullish = state->bullish;
        trend.valid = true;
        outcome.trends.push_back(trend);

        // 3. Direction flip detection, evaluated BEFORE the cache update
        //    (track.lastBullish must still hold the previous value).
        const bool flipped = track.hasState && track.lastBullish != state->bullish;
        const bool flippedToBull = flipped && state->bullish;
        if (flipped)
        {
            anyChanged = true;
        }

        // 4. Entry signal: the 1m timeframe flipping direction. Long on
        //    bear→bull, short on bull→bear. TP/SL is a fixed point distance
        //    (long: TP = price + tpPoints, SL = price − slPoints; short is
        //    mirrored).
        const bool flippedToBear = flipped && false == state->bullish;
        if (track.label == QLatin1String("1m") && (flippedToBull || flippedToBear))
        {
            if (m_lastSignalMs > 0 && nowMs - m_lastSignalMs < cooldownMs)
            {
                AdapterDecision d;
                d.shouldOpen = false;
                d.reason = QStringLiteral("信号冷却中（剩余 %1 秒）")
                               .arg((cooldownMs - (nowMs - m_lastSignalMs)) / 1000);
                outcome.decision = d;
            }
            else
            {
                AdapterDecision d;
                d.shouldOpen = true;
                d.side = flippedToBull ? QStringLiteral("long") : QStringLiteral("short");
                d.entryPrice = price;
                d.confidence = std::clamp(std::abs(price - state->value) / state->atr,
                                          0.0, 1.0);
                if (flippedToBull)
                {
                    d.takeProfit = price + tpPoints;
                    d.stopLoss = price - slPoints;
                }
                else
                {
                    d.takeProfit = price - tpPoints;
                    d.stopLoss = price + slPoints;
                }
                m_lastSignalMs = nowMs;
                d.reason = flippedToBull
                               ? QStringLiteral("SuperTrend 1m 翻多（ST %1 → 价格 %2 上穿）")
                                     .arg(state->value, 0, 'f', 2)
                                     .arg(price, 0, 'f', 2)
                               : QStringLiteral("SuperTrend 1m 翻空（ST %1 → 价格 %2 下穿）")
                                     .arg(state->value, 0, 'f', 2)
                                     .arg(price, 0, 'f', 2);
                outcome.decision = d;
            }
        }

        // 5. Cache the state for the next pass — the flip detection on the
        //    next tick compares against these values.
        track.lastSupertrend = state->value;
        track.lastAtr = state->atr;
        track.lastBullish = state->bullish;
        track.hasState = true;
    }

    outcome.trendChanged = anyChanged;
    return outcome;
}


int EngineStrategyAdapter::klineCount() const
{
    return m_tracks.size() > 1 ? static_cast<int>(m_tracks[1].klines.size()) : 0;
}

} // namespace pulse::ui
