// strategy_registry.cpp — Strategy name → factory registry with fallback (Layer 6)

#include "strategy/StrategyRegistry.hpp"

#include "logging/Logger.hpp"
#include "strategy/scalping/MeanReversionScalper.hpp"
#include "strategy/scalping/MomentumScalper.hpp"
#include "strategy/scalping/OrderBookScalper.hpp"
#include "strategy/scalping/SuperTrendScalper.hpp"
#include "strategy/scalping/UnifiedScalper.hpp"

#include <sstream>

namespace pulse::strategy
{

bool StrategyRegistry::registerFactory(std::string name, Factory factory)
{
    if (m_factories.contains(name))
    {
        PULSE_LOG_WARN("strategy",
            "Strategy name '{}' already registered — keeping the first factory "
            "(a silent overwrite could reroute live orders)",
            name);
        return false;
    }
    m_factories.emplace(std::move(name), std::move(factory));
    return true;
}

std::unique_ptr<StrategyBase> StrategyRegistry::create(const std::string &name,
    const StrategyContext &ctx) const
{
    const auto it = m_factories.find(name);
    if (it != m_factories.end())
    {
        return it->second(ctx);
    }

    // Fallback: passive UnifiedScalper — never emits signals (default
    // evaluateEntry → nullopt). Logs loudly so a typo'd name is noticed.
    std::ostringstream oss;
    const auto names = registeredNames();
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (0 < i)
        {
            oss << ", ";
        }
        oss << names[i];
    }
    PULSE_LOG_WARN("strategy",
        "Unknown strategy name '{}', falling back to UnifiedScalper "
        "(passive — no signals). Registered names: {}",
        name, oss.str());

    return std::make_unique<UnifiedScalper>(ctx);
}

std::vector<std::string> StrategyRegistry::registeredNames() const
{
    std::vector<std::string> names;
    names.reserve(m_factories.size());
    for (const auto &entry : m_factories)
    {
        names.push_back(entry.first);
    }
    return names;
}

StrategyRegistry makeBuiltinStrategyRegistry()
{
    StrategyRegistry registry;
    registry.registerStrategy<MomentumScalper>("momentum_scalper");
    registry.registerStrategy<MeanReversionScalper>("mean_reversion_scalper");
    registry.registerStrategy<SuperTrendScalper>("supertrend_scalper");
    registry.registerStrategy<OrderBookScalper>("orderbook_scalper");
    return registry;
}

} // namespace pulse::strategy
