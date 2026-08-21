// config_validator.cpp — Semantic validation for PulseConfig
//
// Checks business-logic constraints independent of TOML syntax:
//   - Required fields non-empty (exchange credentials, symbols list)
//   - Numeric parameters within safe ranges
//   - Cross-field consistency (strategy symbols ⊆ top-level symbols)
//
// Call after loadConfigFile() or buildDefaultConfig() before starting
// the trading engine.

#include "core/TimeUtil.hpp"
#include "core/config_validator.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace pulse
{

PulseError validateConfig(const PulseConfig &cfg)
{
    // -----------------------------------------------------------------------
    // 1. Symbols list
    // -----------------------------------------------------------------------
    if (cfg.symbols.empty())
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "symbols list must not be empty"};
    }

    // -----------------------------------------------------------------------
    // 2. Exchange credentials
    // -----------------------------------------------------------------------
    if (cfg.exchange.apiKey.empty())
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "exchange.apiKey must not be empty"};
    }

    if (cfg.exchange.apiSecret.empty())
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "exchange.apiSecret must not be empty"};
    }

    if (0 == cfg.exchange.restTimeoutMs)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "exchange.restTimeoutMs must be > 0"};
    }

    // -----------------------------------------------------------------------
    // 3. Risk parameter ranges
    // -----------------------------------------------------------------------
    if (cfg.risk.maxPositionNotional <= 0.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxPositionNotional must be > 0"};
    }

    if (cfg.risk.maxOpenPositions < 1)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxOpenPositions must be >= 1"};
    }

    if (cfg.risk.maxDailyDrawdown <= 0.0 || cfg.risk.maxDailyDrawdown > 1.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxDailyDrawdown must be in (0.0, 1.0]"};
    }

    if (cfg.risk.maxDrawdown <= 0.0 || cfg.risk.maxDrawdown > 1.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxDrawdown must be in (0.0, 1.0]"};
    }

    if (0 == cfg.risk.maxOrdersPerSec)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxOrdersPerSec must be > 0"};
    }

    if (cfg.risk.maxSymbolNotional <= 0.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxSymbolNotional must be > 0"};
    }

    // Optional per-market notional overrides — must be positive when set.
    if (cfg.risk.maxPositionNotionalFutures.has_value()
        && cfg.risk.maxPositionNotionalFutures.value() <= 0.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxPositionNotionalFutures must be > 0"};
    }

    if (cfg.risk.maxPositionNotionalCfd.has_value()
        && cfg.risk.maxPositionNotionalCfd.value() <= 0.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxPositionNotionalCfd must be > 0"};
    }

    if (cfg.risk.maxPositionNotionalSpot.has_value()
        && cfg.risk.maxPositionNotionalSpot.value() <= 0.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.maxPositionNotionalSpot must be > 0"};
    }

    // Optional minimum-free-margin-after-stop gate — must be non-negative.
    if (cfg.risk.minAvailableAfterStopUsd.has_value()
        && cfg.risk.minAvailableAfterStopUsd.value() < 0.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.minAvailableAfterStopUsd must be >= 0"};
    }

    // Futures-specific risk limits (CFD leverage ladder goes up to 500).
    if (cfg.risk.max_leverage < 1.0 || cfg.risk.max_leverage > 500.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.max_leverage must be in [1.0, 500.0]"};
    }

    if (cfg.risk.max_margin_used < 0.0 || cfg.risk.max_margin_used > 1.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.max_margin_used must be in [0.0, 1.0]"};
    }

    // -----------------------------------------------------------------------
    // 4. Stop-loss parameters
    // -----------------------------------------------------------------------
    if (cfg.risk.stop_loss.fixed_pct <= 0.0
        || cfg.risk.stop_loss.fixed_pct > 0.5)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.stop_loss.fixed_pct must be in (0.0, 0.5]"};
    }

    if (cfg.risk.stop_loss.trailing_pct <= 0.0
        || cfg.risk.stop_loss.trailing_pct > 0.5)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.stop_loss.trailing_pct must be in (0.0, 0.5]"};
    }

    // -----------------------------------------------------------------------
    // 5. Take-profit consistency
    // -----------------------------------------------------------------------
    if (cfg.risk.take_profit.enabled)
    {
        if (cfg.risk.take_profit.targets_pct.size()
            != cfg.risk.take_profit.fractions.size())
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                "risk.take_profit.targets_pct and fractions must have "
                "the same length"};
        }

        double fraction_sum = 0.0;

        for (double f : cfg.risk.take_profit.fractions)
        {
            fraction_sum += f;
        }

        if (fraction_sum > 1.0 + 1e-9)
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                "risk.take_profit.fractions must sum to <= 1.0 (got "
                    + std::to_string(fraction_sum) + ")"};
        }
    }

    // -----------------------------------------------------------------------
    // 6. Strategy instances
    // -----------------------------------------------------------------------
    if (cfg.strategy.strategies.empty())
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "at least one strategy instance must be defined"};
    }

    for (std::size_t i = 0; i < cfg.strategy.strategies.size(); ++i)
    {
        const auto &s = cfg.strategy.strategies[i];
        std::string prefix =
            "strategy.instances[" + std::to_string(i) + "]";

        if (s.name.empty())
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              prefix + ".name must not be empty"};
        }

        if (s.symbol.empty())
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              prefix + ".symbol must not be empty"};
        }

        // Symbol must appear in top-level symbols list.
        bool found = false;

        for (const auto &sym : cfg.symbols)
        {
            if (sym == s.symbol)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                prefix + ".symbol \"" + s.symbol
                    + "\" not found in top-level symbols list"};
        }

        if (s.order_quantity <= 0.0)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              prefix + ".order_quantity must be > 0"};
        }

        if (s.min_confidence < 0.0 || s.min_confidence > 1.0)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              prefix + ".min_confidence must be in [0.0, 1.0]"};
        }

        // Futures-specific: leverage must be >= 1.0 and <= risk.max_leverage.
        if (s.leverage < 1.0)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              prefix + ".leverage must be >= 1.0"};
        }

        if (s.leverage > cfg.risk.max_leverage)
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                prefix + ".leverage (" + std::to_string(s.leverage)
                    + ") exceeds risk.max_leverage ("
                    + std::to_string(cfg.risk.max_leverage) + ")"};
        }

        // Testnet only supports futures — spot has no testnet endpoint and
        // CFD (TradFi) has no testnet sandbox.
        if (cfg.exchange.testnet
            && (MarketType::Spot == s.market_type
                || MarketType::Cfd == s.market_type))
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                prefix + ".market_type \"" + toString(s.market_type)
                    + "\" is not supported in testnet "
                    "mode (Gate.io testnet is futures-only; CFD has no "
                    "testnet sandbox). "
                    "Set market_type = \"futures\" or testnet = false."};
        }

        // Post-only / maker-first need an order book and exchange-side
        // post-only support. TradFi CFD only accepts price_type
        // "market"|"trigger" (docs/CFD_TRADFI.md) — no post-only orders.
        if (OrderType::Market != s.order_type && MarketType::Cfd == s.market_type)
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                prefix + ".order_type \"" + toString(s.order_type)
                    + "\" is not supported on market_type \"cfd\" (TradFi "
                      "API has no post-only orders; only \"market\" is "
                      "allowed)"};
        }

        // maker_first must know when to give up and take liquidity.
        if (OrderType::MakerFirst == s.order_type && 0 == s.maker_timeout_ms)
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                prefix + ".maker_timeout_ms must be > 0 when "
                        "order_type = \"maker_first\""};
        }

        // OrderBookScalper needs the WS order-book channel — CFD has none.
        if ("orderbook_scalper" == s.name && MarketType::Cfd == s.market_type)
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                prefix
                    + " strategy \"orderbook_scalper\" cannot run on "
                      "market_type \"cfd\" (CFD has no order-book channel). "
                      "Use momentum_scalper, mean_reversion_scalper or "
                      "supertrend_scalper."};
        }
    }

    // -----------------------------------------------------------------------
    // 7. Aggregator threshold
    // -----------------------------------------------------------------------
    if (cfg.strategy.signal_aggregator_threshold < 0.0
        || cfg.strategy.signal_aggregator_threshold > 1.0)
    {
        return PulseError{
            ErrorCode::ConfigValidationError,
            "strategy.signal_aggregator_threshold must be in [0.0, 1.0]"};
    }

    // -----------------------------------------------------------------------
    // 8. AI config (only validate when enabled via heartbeat > 0)
    // -----------------------------------------------------------------------
    if (cfg.ai.heartbeatIntervalSec > 0)
    {
        if (cfg.ai.stats_lookback_hours < 1)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "ai.stats_lookback_hours must be ≥ 1 when AI is enabled"};
        }

        if (cfg.ai.apiKey.empty())
        {
            // No key → the AI stays offline (main.cpp gates the scheduler on
            // !apiKey.empty()). The config remains valid so the key can be
            // dropped into .env later without touching the toml.
            return {};
        }

        if ("openai" != cfg.ai.backend && "claude" != cfg.ai.backend)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "ai.backend must be \"openai\" or \"claude\""};
        }

        if (cfg.ai.model.empty())
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "ai.model must not be empty when AI is enabled"};
        }
    }

    // -----------------------------------------------------------------------
    // 9. Log level
    // -----------------------------------------------------------------------
    static const std::vector<std::string> kValidLevels = {
        "trace", "debug", "info", "warn", "error", "critical", "off"};
    bool level_ok = false;

    for (const auto &lvl : kValidLevels)
    {
        if (lvl == cfg.log.level)
        {
            level_ok = true;
            break;
        }
    }

    if (!level_ok)
    {
        return PulseError{
            ErrorCode::ConfigValidationError,
            "log.level must be one of: trace, debug, info, warn, "
            "error, critical, off (got \""
                + cfg.log.level + "\")"};
    }

    // -----------------------------------------------------------------------
    // 10. SQLite config (only validate when enabled)
    // -----------------------------------------------------------------------
    if (cfg.sqlite.enabled)
    {
        if (cfg.sqlite.dbPath.empty())
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "sqlite.dbPath must not be empty when "
                              "sqlite.enabled is true"};
        }
    }

    // -----------------------------------------------------------------------
    // 11. Control plane config (only validate when enabled)
    // -----------------------------------------------------------------------
    if (cfg.control.enabled)
    {
        if (0 == cfg.control.port)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "control.port must be in [1, 65535] (got "
                                  + std::to_string(cfg.control.port) + ")"};
        }
        if (cfg.control.bindAddress.empty())
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "control.bindAddress must not be empty when "
                              "control.enabled is true"};
        }
        if (!parseDisplayTimezone(cfg.control.displayTimezone))
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "control.display_timezone must be \"local\", "
                              "\"utc\" or \"±HH:MM\" (got \""
                                  + cfg.control.displayTimezone + "\")"};
        }
    }

    // [grid] section (M27 grid service). Validated when enabled — the grid
    // trades real money on a futures contract, so bad geometry must fail
    // loudly at startup, not mid-run.
    if (cfg.grid.enabled)
    {
        if (cfg.grid.symbol.empty())
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "grid.symbol must not be empty when grid.enabled"};
        }
        if (cfg.grid.market_type != "futures")
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "grid.market_type must be \"futures\" (got \""
                                  + cfg.grid.market_type + "\")"};
        }
        if (cfg.grid.levels < 1 || cfg.grid.levels > 24)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "grid.levels must be in [1, 24] (got "
                                  + std::to_string(cfg.grid.levels) + ")"};
        }
        if (cfg.grid.qty_per_level <= 0.0)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "grid.qty_per_level must be > 0"};
        }
        if (cfg.grid.step_mode != "atr" && cfg.grid.step_mode != "fixed")
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "grid.step_mode must be \"atr\" or \"fixed\" "
                              "(got \"" + cfg.grid.step_mode + "\")"};
        }
        if (cfg.grid.step_mode == "atr"
            && (cfg.grid.step_min <= 0.0 || cfg.grid.step_max < cfg.grid.step_min))
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "grid.step_min/step_max invalid for atr mode"};
        }
        if (cfg.grid.step_mode == "fixed" && cfg.grid.step_fixed <= 0.0)
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "grid.step_fixed must be > 0 in fixed mode"};
        }
    }

    return {}; // All checks passed.
}

} // namespace pulse
