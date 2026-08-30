// gold_strategy_engine.cpp — implementation of the v1 gold strategy engine.

#include "GoldStrategyEngine.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pulse::ui
{

namespace
{

// Keep at most this many ticks of history (slow EMA + vol window + margin).
constexpr std::size_t kMaxHistory = 200;

} // namespace

GoldStrategyEngine::GoldStrategyEngine(std::shared_ptr<const Params> params)
    : m_params(std::move(params))
{
}

// ---------------------------------------------------------------------------
// EMA over the stored history (fresh full-series computation, seeded SMA —
// deterministic, no drifting rolling state).
// ---------------------------------------------------------------------------
double GoldStrategyEngine::volatility() const
{
    const int window = m_params->volWindow.load();
    if (window <= 0 || static_cast<std::size_t>(window) > m_prices.size())
    {
        return 0.0;
    }
    const std::size_t start = m_prices.size() - static_cast<std::size_t>(window);
    const auto [lo, hi] = std::minmax_element(
        m_prices.begin() + static_cast<std::ptrdiff_t>(start), m_prices.end());
    return *hi - *lo;
}

double GoldStrategyEngine::ema(int period) const
{
    if (period <= 0 || static_cast<std::size_t>(period) > m_prices.size())
    {
        return 0.0;
    }
    const double k = 2.0 / (period + 1.0);

    // Seed with the SMA of the first `period` closes.
    const std::size_t start = m_prices.size() - static_cast<std::size_t>(period);
    double sum = 0.0;
    for (std::size_t i = start; i < m_prices.size(); ++i)
    {
        sum += m_prices[i];
    }
    double value = sum / period;

    // Recursive smoothing over the remaining history (older → newer).
    for (std::size_t i = 0; i < start; ++i)
    {
        value = k * m_prices[i] + (1.0 - k) * value;
    }
    return value;
}

GoldDecision GoldStrategyEngine::onPrice(double price, std::int64_t nowMs)
{
    GoldDecision decision;
    decision.entryPrice = price;

    // 1. Buffer management.
    if (m_prices.size() == kMaxHistory)
    {
        m_prices.erase(m_prices.begin());
    }
    m_prices.push_back(price);

    // 2. Warm-up: the slow EMA and the volatility window need history.
    const std::size_t needed = static_cast<std::size_t>(m_params->emaSlow.load() + m_params->volWindow.load());
    if (m_prices.size() < needed)
    {
        decision.reason = QStringLiteral("预热中（%1/%2）")
                              .arg(m_prices.size())
                              .arg(needed);
        return decision;
    }

    // 3. Volatility envelope (peak-to-trough of the last volWindow ticks).
    const double vol = volatility();
    if (vol < m_params->minVolUsd.load())
    {
        decision.reason = QStringLiteral("波幅过低（%1 USD < 下限 %2），市场停滞")
                              .arg(vol, 0, 'f', 2)
                              .arg(m_params->minVolUsd.load(), 0, 'f', 2);
        return decision;
    }
    if (vol > m_params->maxVolUsd.load())
    {
        decision.reason = QStringLiteral("波幅过大（%1 USD > 上限 %2），疑似插针")
                              .arg(vol, 0, 'f', 2)
                              .arg(m_params->maxVolUsd.load(), 0, 'f', 2);
        return decision;
    }

    // 4. Trend gate: long entries only in an upward EMA regime.
    const double fast = ema(m_params->emaFast.load());
    const double slow = ema(m_params->emaSlow.load());
    if (fast <= slow)
    {
        decision.reason = QStringLiteral("趋势偏空（EMA%1 %2 ≤ EMA%3 %4），不开多")
                              .arg(m_params->emaFast.load())
                              .arg(fast, 0, 'f', 2)
                              .arg(m_params->emaSlow.load())
                              .arg(slow, 0, 'f', 2);
        return decision;
    }

    // 5. Cooldown gate: one open signal per cooldown window.
    if (m_lastSignalMs > 0 && nowMs - m_lastSignalMs < m_params->cooldownMs.load())
    {
        decision.reason = QStringLiteral("信号冷却中（剩余 %1 秒）")
                              .arg((m_params->cooldownMs.load() - (nowMs - m_lastSignalMs)) / 1000);
        return decision;
    }

    // 6. Open: entry at market, TP/SL from the volatility envelope.
    decision.shouldOpen = true;
    decision.takeProfit = price + m_params->tpVolMult.load() * vol;
    decision.stopLoss = price - m_params->slVolMult.load() * vol;
    decision.volumeLots = m_params->openVolumeLots.load();
    m_lastSignalMs = nowMs;
    decision.reason = QStringLiteral("EMA 上行共振，波幅 %1 USD → 开多")
                          .arg(vol, 0, 'f', 2);
    return decision;
}

std::optional<GoldStrategyEngine::GoldStops> GoldStrategyEngine::suggestStops(double price) const
{
    // Warm-up and volatility bounds mirror the open-decision gates: without
    // a valid envelope there is nothing sensible to suggest.
    const std::size_t needed =
        static_cast<std::size_t>(m_params->emaSlow.load() + m_params->volWindow.load());
    if (m_prices.size() < needed)
    {
        return std::nullopt;
    }
    const double vol = volatility();
    if (vol < m_params->minVolUsd.load() || vol > m_params->maxVolUsd.load())
    {
        return std::nullopt;
    }
    GoldStops stops;
    stops.takeProfit = price + m_params->tpVolMult.load() * vol;
    stops.stopLoss = price - m_params->slVolMult.load() * vol;
    return stops;
}

void GoldStrategyEngine::reset()
{
    m_prices.clear();
    m_lastSignalMs = 0;
}

int GoldStrategyEngine::priceCount() const
{
    return static_cast<int>(m_prices.size());
}

} // namespace pulse::ui
