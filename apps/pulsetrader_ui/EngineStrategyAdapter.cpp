// engine_strategy_adapter.cpp — implementation of the strategy adapter.

#include "EngineStrategyAdapter.hpp"

#include "VerifyWorker.hpp"

#include "strategy/scalping/EmaResonanceScalper.hpp"

#include "exchange/GateRestClient.hpp"

#include "market/KlineBuffer.hpp"  // pulse::market::Kline (POD struct)

#include <algorithm>
#include <exception>

namespace pulse::ui
{

// Lift the protected evaluateEntry() of the engine's resonance strategy to
// public so the adapter can drive it directly (the template-method base
// keeps it protected for engine-internal callers; the adapter is an
// external consumer and never calls onKline/onTick).
class ExposedResonance : public pulse::strategy::EmaResonanceScalper
{
  public:
    using EmaResonanceScalper::EmaResonanceScalper;
    using EmaResonanceScalper::evaluateEntry;
};

namespace
{

// Candle history cap (slowest EMA 200 + margin).
constexpr std::size_t kMaxCandles = 300;
// 1m candle in milliseconds.
constexpr std::int64_t kMinuteMs = 60'000;

// Lenient parse of one TradFi kline object → Kline. Returns false when the
// required fields are missing.
bool parseCfdKline(const nlohmann::json &obj, pulse::market::Kline &out)
{
    if (false == obj.is_object())
    {
        return false;
    }
    const auto strOf = [&obj](const char *key) -> std::optional<double> {
        if (false == obj.contains(key))
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
    };
    const auto open = strOf("open");
    const auto high = strOf("high");
    const auto low = strOf("low");
    const auto close = strOf("close");
    if (false == open.has_value() || false == high.has_value()
        || false == low.has_value() || false == close.has_value())
    {
        return false;
    }
    out.open = *open;
    out.high = *high;
    out.low = *low;
    out.close = *close;
    out.volume = strOf("volume").value_or(0.0);

    // Timestamp field: "timestamp" (seconds or ms) or "open_time" (ms).
    std::int64_t tMs = 0;
    if (obj.contains("timestamp"))
    {
        const auto t = strOf("timestamp");
        if (t.has_value())
        {
            tMs = *t < 1'000'000'000'000LL ? static_cast<std::int64_t>(*t * 1000.0)
                                           : static_cast<std::int64_t>(*t);
        }
    }
    else if (obj.contains("open_time"))
    {
        const auto t = strOf("open_time");
        if (t.has_value())
        {
            tMs = static_cast<std::int64_t>(*t);
        }
    }
    out.open_time = tMs;
    out.close_time = tMs + kMinuteMs;
    out.closed = true;
    return tMs > 0;
}

} // namespace

EngineStrategyAdapter::EngineStrategyAdapter()
{
    // The strategy only touches the injected context inside onKline/onTick
    // (which we never call) — evaluateEntry works purely on the kline
    // snapshot, so a default (all-null) context is safe.
    m_strategy = std::make_unique<ExposedResonance>(pulse::strategy::StrategyContext{});
}

EngineStrategyAdapter::~EngineStrategyAdapter() = default;

int EngineStrategyAdapter::warmup(pulse::exchange::GateRestClient &cfd)
{
    const auto klines = cfd.getCfdKlines("XAUUSD", 500);
    if (false == pulse::ok(klines))
    {
        return 0;
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
        return 0;
    }

    m_klines.clear();
    for (const auto &item : *list)
    {
        pulse::market::Kline kline;
        if (parseCfdKline(item, kline))
        {
            m_klines.push_back(kline);
        }
    }
    return static_cast<int>(m_klines.size());
}

std::optional<AdapterDecision> EngineStrategyAdapter::onTick(double price,
                                                             std::int64_t nowMs,
                                                             double tpMult,
                                                             double slMult,
                                                             std::int64_t cooldownMs)
{
    // 1. Aggregate the tick into the current 1m candle.
    const std::int64_t bucket = nowMs / kMinuteMs * kMinuteMs;
    if (m_currentBucketMs < 0 || bucket != m_currentBucketMs)
    {
        // Close the previous candle and open a new one.
        if (false == m_klines.empty() && false == m_klines.back().closed)
        {
            m_klines.back().closed = true;
        }
        pulse::market::Kline fresh;
        fresh.open_time = bucket;
        fresh.close_time = bucket + kMinuteMs;
        fresh.open = price;
        fresh.high = price;
        fresh.low = price;
        fresh.close = price;
        fresh.closed = false;
        m_klines.push_back(fresh);
        if (m_klines.size() > kMaxCandles)
        {
            m_klines.erase(m_klines.begin());
        }
        m_currentBucketMs = bucket;
    }
    else
    {
        auto &cur = m_klines.back();
        cur.high = std::max(cur.high, price);
        cur.low = std::min(cur.low, price);
        cur.close = price;
    }

    // 2. Warm-up check: the strategy needs ~201 candles for its EMA seed.
    const std::size_t needed = 201;
    if (m_klines.size() < needed)
    {
        AdapterDecision d;
        d.shouldOpen = false;
        d.reason = QStringLiteral("K线预热中（%1/%2）")
                       .arg(m_klines.size())
                       .arg(needed);
        return d;
    }

    // 3. Cooldown gate runs BEFORE evaluation: inside the window the whole
    //    evaluation is skipped (no state commit), so the strategy re-enters
    //    from its pre-signal state once the window elapses. A transition
    //    right after a signal therefore cannot open a second position.
    if (m_lastSignalMs > 0 && nowMs - m_lastSignalMs < cooldownMs)
    {
        AdapterDecision d;
        d.shouldOpen = false;
        d.reason = QStringLiteral("信号冷却中（剩余 %1 秒）")
                       .arg((cooldownMs - (nowMs - m_lastSignalMs)) / 1000);
        return d;
    }

    // 4. Evaluate the engine strategy on the full snapshot.
    const auto entry = m_strategy->evaluateEntry(m_klines);
    if (false == entry.has_value() || pulse::strategy::SignalType::Buy != entry->type)
    {
        AdapterDecision d;
        d.shouldOpen = false;
        d.reason = QString::fromStdString(
            entry.has_value() ? entry->reason : "EMA 共振未对齐（mixed）");
        return d;
    }

    // 5. Buy entry: price = latest close; TP/SL from the ATR envelope.
    AdapterDecision d;
    d.shouldOpen = true;
    d.entryPrice = entry->price;
    d.confidence = entry->confidence;
    d.takeProfit = entry->price + tpMult * entry->atr;
    d.stopLoss = entry->price - slMult * entry->atr;
    m_lastSignalMs = nowMs;
    d.reason = QStringLiteral("EMA 五线共振转多，置信度 %1，ATR %2")
                   .arg(entry->confidence, 0, 'f', 3)
                   .arg(entry->atr, 0, 'f', 2);
    return d;
}

std::optional<EngineStrategyAdapter::Stops> EngineStrategyAdapter::suggestStops(
    double price, double tpMult, double slMult) const
{
    // ATR14 over the buffered candles (true range, Wilder-style smoothing
    // approximated by a plain average — deterministic and stateless).
    constexpr std::size_t kAtrPeriod = 14;
    if (m_klines.size() <= kAtrPeriod)
    {
        return std::nullopt;
    }
    double sum = 0.0;
    const std::size_t start = m_klines.size() - kAtrPeriod;
    for (std::size_t i = start; i < m_klines.size(); ++i)
    {
        const auto &k = m_klines[i];
        const double prevClose = (i > 0) ? m_klines[i - 1].close : k.open;
        const double tr = std::max(k.high - k.low,
                                   std::max(std::abs(k.high - prevClose),
                                            std::abs(k.low - prevClose)));
        sum += tr;
    }
    const double atr = sum / static_cast<double>(kAtrPeriod);
    if (atr <= 0.0)
    {
        return std::nullopt;
    }
    Stops stops;
    stops.takeProfit = price + tpMult * atr;
    stops.stopLoss = price - slMult * atr;
    return stops;
}

int EngineStrategyAdapter::klineCount() const
{
    return static_cast<int>(m_klines.size());
}

} // namespace pulse::ui
