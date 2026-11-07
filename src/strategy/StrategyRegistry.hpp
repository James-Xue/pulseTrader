#pragma once
// strategy_registry.hpp — Strategy name → factory registry with fallback (Layer 6)
//
// Replaces the hard-coded if-chain in apps/pulsetrader/main.cpp:
//   - TOML `name` is the registration key ("momentum_scalper", "eth_scalper", …)
//   - create() never returns nullptr: an UNREGISTERED name falls back to a
//     passive UnifiedScalper (its default evaluateEntry returns nullopt, so
//     it never emits signals) and logs a WARN listing the registered names.
//     A passive instance doubles as a data-connectivity canary on the signal
//     board — a typo'd name no longer kills the engine ("No strategies
//     registered. Exiting."), it just runs a silent watcher.
//   - Duplicate registration is rejected (returns false, WARN) — the first
//     registration wins; a silent overwrite could reroute live orders.
//
// Registry is intentionally NOT a static-registration macro/table: the
// project lists sources explicitly in CMakeLists (no glob), and one central
// makeBuiltinStrategyRegistry() keeps new strategies reviewable in one spot.

#include "strategy/StrategyBase.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// StrategyRegistry — name-keyed strategy factories with fallback
// ---------------------------------------------------------------------------
class StrategyRegistry
{
  public:
    /// Factory signature: build a strategy for the given context.
    using Factory = std::function<std::unique_ptr<StrategyBase>(const StrategyContext &)>;

    /// Register a factory under `name`. Returns false (and logs a WARN) when
    /// the name is already registered — first registration wins.
    bool registerFactory(std::string name, Factory factory);

    /// Convenience: register any StrategyBase subclass by name.
    template <typename T>
    bool registerStrategy(const std::string &name)
    {
        return registerFactory(name, [](const StrategyContext &ctx)
            {
                return std::make_unique<T>(ctx);
            });
    }

    /// Create a strategy by name. Unregistered names → WARN + passive
    /// UnifiedScalper. Never returns nullptr.
    std::unique_ptr<StrategyBase> create(const std::string &name,
        const StrategyContext &ctx) const;

    /// Sorted list of registered names (for the fallback WARN message).
    [[nodiscard]] std::vector<std::string> registeredNames() const;

  private:
    std::map<std::string, Factory> m_factories;
};

/// The built-in registry: momentum / mean_reversion / supertrend / orderbook
/// (plus per-coin strategies as they land — see EthScalper).
StrategyRegistry makeBuiltinStrategyRegistry();

} // namespace pulse::strategy
