// strategy_params.cpp — Canonical atomic-param key registry (M33)
//
// One source of truth for the hot-reloadable StrategyParams fields so the
// backtest / sweep tooling can classify a raw "--param KEY=VALUE" override:
//   - key ∈ atomicParamKeys()     → written into the StrategyParams atomics
//   - any other key               → custom_params channel (strategy-specific
//                                   keys like eth_atr_step / res_ema_p1..p5)
//
// The live control-plane getter/setter tables live in
// src/control/EngineServices.cpp — they must stay in sync with this list
// (tests/unit/strategy pins both sets).

#include "strategy/StrategyParams.hpp"

namespace pulse::strategy
{

const std::vector<std::string> &atomicParamKeys()
{
    static const std::vector<std::string> keys = {
        "order_quantity",
        "min_confidence",
        "ema_fast_period",
        "ema_slow_period",
        "bb_period",
        "bb_std_dev",
        "ob_imbalance_threshold",
        "ob_depth",
        "supertrend_period",
        "supertrend_multiplier",
        "cooldown_seconds",
        "stop_loss_pct",
        "take_profit_pct",
        "auto_trade",
    };
    return keys;
}

bool applyAtomicParam(StrategyParams &params, const std::string &key, double value)
{
    if ("order_quantity" == key)
    {
        params.order_quantity.store(value);
    }
    else if ("min_confidence" == key)
    {
        params.min_confidence.store(value);
    }
    else if ("ema_fast_period" == key)
    {
        params.ema_fast_period.store(value);
    }
    else if ("ema_slow_period" == key)
    {
        params.ema_slow_period.store(value);
    }
    else if ("bb_period" == key)
    {
        params.bb_period.store(value);
    }
    else if ("bb_std_dev" == key)
    {
        params.bb_std_dev.store(value);
    }
    else if ("ob_imbalance_threshold" == key)
    {
        params.ob_imbalance_threshold.store(value);
    }
    else if ("ob_depth" == key)
    {
        params.ob_depth.store(value);
    }
    else if ("supertrend_period" == key)
    {
        params.supertrend_period.store(value);
    }
    else if ("supertrend_multiplier" == key)
    {
        params.supertrend_multiplier.store(value);
    }
    else if ("cooldown_seconds" == key)
    {
        params.cooldown_seconds.store(value);
    }
    else if ("stop_loss_pct" == key)
    {
        params.stop_loss_pct.store(value);
    }
    else if ("take_profit_pct" == key)
    {
        params.take_profit_pct.store(value);
    }
    else if ("auto_trade" == key)
    {
        params.auto_trade.store(value);
    }
    else
    {
        return false;
    }
    return true;
}

} // namespace pulse::strategy
