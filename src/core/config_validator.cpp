// config_validator.cpp — Semantic validation for PulseConfig
//
// Checks business-logic constraints independent of TOML syntax:
//   - Required fields non-empty (exchange credentials, symbols list)
//   - Numeric parameters within safe ranges
//   - Cross-field consistency (strategy symbols ⊆ top-level symbols)
//
// Call after load_config_file() or build_default_config() before starting
// the trading engine.

#include "core/config_validator.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace pulse
{

PulseError validate_config(const PulseConfig &cfg)
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

    // Futures-specific risk limits.
    if (cfg.risk.max_leverage < 1.0 || cfg.risk.max_leverage > 125.0)
    {
        return PulseError{ErrorCode::ConfigValidationError,
                          "risk.max_leverage must be in [1.0, 125.0]"};
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

        // Testnet only supports futures — spot has no testnet endpoint.
        if (cfg.exchange.testnet && MarketType::Spot == s.market_type)
        {
            return PulseError{
                ErrorCode::ConfigValidationError,
                prefix + ".market_type \"spot\" is not supported in testnet "
                    "mode (Gate.io testnet is futures-only). "
                    "Set market_type = \"futures\" or testnet = false."};
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

        if (cfg.ai.apiKey.empty())
        {
            return PulseError{ErrorCode::ConfigValidationError,
                              "ai.apiKey must not be empty when AI is enabled"};
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

    return {}; // All checks passed.
}

} // namespace pulse
