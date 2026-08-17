#include "strategy/signal/SignalBoard.hpp"

#include <chrono>
#include <mutex>

namespace pulse::strategy
{

namespace
{

// SignalType → lowercase wire string ("buy" / "sell" / "flat").
const char *signalTypeToString(SignalType type)
{
    switch (type)
    {
    case SignalType::Buy:
        return "buy";
    case SignalType::Sell:
        return "sell";
    case SignalType::Flat:
        return "flat";
    }
    return "flat";
}

nlohmann::json entryToJson(const SignalBoard::Entry &entry, const std::string &source)
{
    return nlohmann::json{
        { "source", source },
        { "symbol", entry.signal.symbol },
        { "market_type", toString(entry.signal.market_type) },
        { "type", signalTypeToString(entry.signal.type) },
        { "confidence", entry.signal.confidence },
        { "price", entry.signal.price },
        { "reason", entry.signal.reason },
        { "ts_ms", entry.ts_ms },
        { "indicators", entry.signal.indicators },
    };
}

} // anonymous namespace

SignalBoard::SignalBoard(double aggregate_threshold)
    : m_aggregateThreshold{ aggregate_threshold }
{
}

void SignalBoard::publish(const TradingSignal &signal)
{
    if (signal.strategy_id.empty())
    {
        return; // Anonymous signals cannot be keyed.
    }

    const auto now = nowMs();
    std::unique_lock lock{ m_mutex };
    m_latest[signal.strategy_id] = Entry{ signal, now };
}

void SignalBoard::publishAggregate(const TradingSignal &signal)
{
    const auto now = nowMs();
    std::unique_lock lock{ m_mutex };
    m_aggregate = Entry{ signal, now };
}

nlohmann::json SignalBoard::snapshot() const
{
    nlohmann::json signals = nlohmann::json::array();
    nlohmann::json aggregate = nullptr;

    {
        std::shared_lock lock{ m_mutex };
        for (const auto &[strategy_id, entry] : m_latest)
        {
            signals.push_back(entryToJson(entry, strategy_id));
        }
        if (m_aggregate.has_value())
        {
            aggregate = entryToJson(*m_aggregate, "aggregate");
            aggregate["threshold"] = m_aggregateThreshold;
        }
    }

    return nlohmann::json{
        { "signals", signals },
        { "aggregate", aggregate },
    };
}

std::uint64_t SignalBoard::entryCount() const
{
    std::shared_lock lock{ m_mutex };
    return static_cast<std::uint64_t>(m_latest.size());
}

std::int64_t SignalBoard::nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace pulse::strategy
