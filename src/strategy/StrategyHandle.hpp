#pragma once
// strategy_handle.hpp — strategy instance identity + mutable parameter handle
//
// Shared by the AI pipeline (snapshot collection + parameter deltas) and the
// audit layer. Lives in the strategy layer so ai/control can reference it
// without a strategy → ai dependency edge. The params pointer is owned by the
// strategy instance and stays valid for the engine's lifetime.

#include <string>

namespace pulse::strategy
{

class StrategyParams;

struct StrategyHandle
{
    std::string id;          ///< Instance id, e.g. "momentum_scalper_BTC_USDT".
    std::string name;        ///< Class name, e.g. "MomentumScalper".
    std::string symbol;      ///< Trading pair, e.g. "BTC_USDT".
    std::string market_type; ///< "spot" / "futures" / "cfd".
    StrategyParams *params{ nullptr }; ///< Hot-reload atomic parameters.
};

} // namespace pulse::strategy
