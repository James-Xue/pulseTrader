// unified_scalper.cpp — Template-method base for kline-driven scalpers (Layer 6)

#include "strategy/scalping/UnifiedScalper.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pulse::strategy
{

UnifiedScalper::UnifiedScalper(const StrategyContext &context)
{
    m_context = context;
}

std::string UnifiedScalper::name() const
{
    return className();
}

std::string UnifiedScalper::id() const
{
    return idPrefix() + "_" + m_context.config.symbol;
}

StrategyParams &UnifiedScalper::params()
{
    return m_params;
}

void UnifiedScalper::onTick(const market::Ticker & /*ticker*/)
{
    // Kline-driven strategies use onTick() only to detect "no kline data at
    // all" (e.g. WS not connected) — throttled to one log line per 30 s.
    auto *feed = m_context.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->getKlineBuffer(m_context.config.symbol).snapshot(1);
    if (candles.empty())
    {
        logNoDataThrottled();
    }
}

void UnifiedScalper::onOrderbook(const market::OrderBook & /*book*/)
{
    // This strategy family is kline-driven; orderbook updates are ignored.
}

void UnifiedScalper::onKline(const market::Kline & /*kline*/)
{
    // 1. No feed → nothing to evaluate (matches legacy early return).
    auto *feed = m_context.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    // 2. Pull the candles this strategy needs.
    const auto needed = klineNeeded();
    auto candles = feed->getKlineBuffer(m_context.config.symbol).snapshot(needed);

    // 3. Warmup gate — throttled log until enough candles accumulated.
    const auto threshold = warmupThreshold();
    if (candles.size() < threshold)
    {
        logWarmupThrottled(candles.size(), threshold);
        return;
    }

    // 4. Detection + rolling-state commit (subclass hook).
    auto entry = evaluateEntry(candles);
    if (!entry)
    {
        return;
    }

    // 5. Cooldown gate — blocks the signal but NOT the state commit that
    //    evaluateEntry already performed (legacy SuperTrend semantics).
    if (inCooldown())
    {
        return;
    }

    // 6. Build the signal; base fills the identity fields.
    auto signal = buildSignal(*entry);
    signal.symbol = m_context.config.symbol;
    signal.strategy_id = id();
    signal.timestamp = now();

    // 7. Log (subclass format), emit, and stamp the cooldown timestamp.
    logSignal(signal);
    emitSignal(signal);
    m_lastSignalTimeMs = nowMs();
}

std::string UnifiedScalper::className() const
{
    return "UnifiedScalper";
}

std::string UnifiedScalper::idPrefix() const
{
    return m_context.config.name;
}

std::size_t UnifiedScalper::klineNeeded() const
{
    return 1;
}

std::size_t UnifiedScalper::warmupThreshold() const
{
    return klineNeeded();
}

bool UnifiedScalper::cooldownEnabled() const
{
    return true;
}

std::optional<EntryContext> UnifiedScalper::evaluateEntry(
    const std::vector<market::Kline> & /*candles*/)
{
    // Passive default: never emits. The StrategyRegistry falls back to this
    // for unknown strategy names — a passive instance is a data-connectivity
    // canary, not a trading strategy.
    return std::nullopt;
}

TradingSignal UnifiedScalper::buildSignal(const EntryContext &entry) const
{
    TradingSignal signal;
    signal.type = entry.type;
    signal.confidence = entry.confidence;
    signal.price = entry.price;
    signal.reason = entry.reason;
    signal.indicators = entry.indicators;
    return signal;
}

void UnifiedScalper::logSignal(const TradingSignal &sig) const
{
    PULSE_LOG_INFO("strategy", "[{}] {} signal: confidence={:.4f}, price={:.2f}",
        id(), sig.reason, sig.confidence, sig.price);
}

double UnifiedScalper::computeAtr(const std::vector<market::Kline> &candles,
    std::size_t period) const
{
    // Need at least period + 1 candles to compute `period` true ranges.
    if (candles.size() < period + 1)
    {
        return 0.0;
    }

    // Compute True Range for the last `period` candles.
    // TR = max(high - low, |high - prev_close|, |low - prev_close|)
    double sum_tr = 0.0;
    const auto start = candles.size() - period;
    for (std::size_t i = start; i < candles.size(); ++i)
    {
        const double hl = candles[i].high - candles[i].low;
        const double hpc = std::abs(candles[i].high - candles[i - 1].close);
        const double lpc = std::abs(candles[i].low - candles[i - 1].close);
        sum_tr += std::max({ hl, hpc, lpc });
    }

    return sum_tr / static_cast<double>(period);
}

double UnifiedScalper::customParam(const std::string &key, double fallback) const
{
    const auto it = m_context.config.custom_params.find(key);
    return (it != m_context.config.custom_params.end()) ? it->second : fallback;
}

double UnifiedScalper::computeEma(const std::vector<double> &closes,
    double period,
    double prev_ema)
{
    if (closes.empty())
    {
        return 0.0;
    }

    const double k = 2.0 / (period + 1.0);

    // On first call (prev_ema == 0.0), seed with SMA of the first `period` closes.
    double ema = prev_ema;
    std::size_t start = 0;

    if (0.0 == prev_ema && closes.size() >= static_cast<std::size_t>(period))
    {
        // Compute SMA seed.
        double sum = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(period); ++i)
        {
            sum += closes[i];
        }
        ema = sum / period;
        start = static_cast<std::size_t>(period);
    }

    // Apply EMA formula for remaining closes.
    for (std::size_t i = start; i < closes.size(); ++i)
    {
        ema = closes[i] * k + ema * (1.0 - k);
    }

    return ema;
}

std::int64_t UnifiedScalper::nowMs() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool UnifiedScalper::inCooldown() const
{
    if (!cooldownEnabled())
    {
        return false;
    }
    const double cooldown_sec = m_params.cooldown_seconds.load(std::memory_order_acquire);
    if (cooldown_sec <= 0.0)
    {
        return false;
    }
    const auto cooldown_ms = static_cast<std::int64_t>(cooldown_sec * 1000.0);
    return (nowMs() - m_lastSignalTimeMs) < cooldown_ms;
}

void UnifiedScalper::logWarmupThrottled(std::size_t have, std::size_t need)
{
    const auto now_ms = nowMs();
    if (now_ms - m_lastWarmupLogMs >= 30'000)
    {
        PULSE_LOG_INFO("strategy",
            "[{}] Warming up: {}/{} candles accumulated (need ~{} min of kline data)",
            id(), have, need, need);
        m_lastWarmupLogMs = now_ms;
    }
}

void UnifiedScalper::logNoDataThrottled()
{
    const auto now_ms = nowMs();
    if (now_ms - m_lastNoDataLogMs >= 30'000)
    {
        PULSE_LOG_INFO("strategy",
            "[{}] Waiting for kline data (WS may not be connected yet)", id());
        m_lastNoDataLogMs = now_ms;
    }
}

} // namespace pulse::strategy
