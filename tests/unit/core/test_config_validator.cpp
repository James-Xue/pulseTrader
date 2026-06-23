// test_config_validator.cpp — Unit tests for PulseConfig semantic validation
//
// Each test constructs a PulseConfig directly (no TOML parsing),
// mutates one field to an invalid value, and asserts the correct error.

#include "core/config_validator.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pulse
{
namespace
{

// Helper: build a minimal valid PulseConfig for mutation in tests.
PulseConfig valid_config()
{
    PulseConfig cfg;
    cfg.symbols = {"BTC_USDT"};
    cfg.exchange.apiKey = "test_key";
    cfg.exchange.apiSecret = "test_secret";

    // Disable AI by default so tests that don't exercise AI pass cleanly.
    cfg.ai.heartbeatIntervalSec = 0;

    StrategyInstanceConfig inst;
    inst.name = "momentum_scalper";
    inst.symbol = "BTC_USDT";
    inst.order_quantity = 0.001;
    inst.min_confidence = 0.6;
    cfg.strategy.strategies.push_back(inst);

    return cfg;
}

// ---------------------------------------------------------------------------
// Valid config passes
// ---------------------------------------------------------------------------

TEST(ConfigValidator, AcceptsValidConfig)
{
    auto cfg = valid_config();
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::Ok, err.code) << err.message;
}

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsEmptySymbols)
{
    auto cfg = valid_config();
    cfg.symbols.clear();
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("symbols"));
}

// ---------------------------------------------------------------------------
// Exchange credentials
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsEmptyApiKey)
{
    auto cfg = valid_config();
    cfg.exchange.apiKey = "";
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("apiKey"));
}

TEST(ConfigValidator, RejectsEmptyApiSecret)
{
    auto cfg = valid_config();
    cfg.exchange.apiSecret = "";
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("apiSecret"));
}

TEST(ConfigValidator, RejectsZeroRestTimeout)
{
    auto cfg = valid_config();
    cfg.exchange.restTimeoutMs = 0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("restTimeoutMs"));
}

// ---------------------------------------------------------------------------
// Risk parameters
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsNegativePositionNotional)
{
    auto cfg = valid_config();
    cfg.risk.maxPositionNotional = -100.0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("maxPositionNotional"));
}

TEST(ConfigValidator, RejectsZeroMaxOpenPositions)
{
    auto cfg = valid_config();
    cfg.risk.maxOpenPositions = 0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("maxOpenPositions"));
}

TEST(ConfigValidator, RejectsDailyDrawdownOutOfRange)
{
    {
        auto cfg = valid_config();
        cfg.risk.maxDailyDrawdown = 0.0;
        auto err = validateConfig(cfg);
        EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    }
    {
        auto cfg = valid_config();
        cfg.risk.maxDailyDrawdown = 1.5;
        auto err = validateConfig(cfg);
        EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    }
}

TEST(ConfigValidator, RejectsDrawdownOutOfRange)
{
    auto cfg = valid_config();
    cfg.risk.maxDrawdown = 2.0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("maxDrawdown"));
}

TEST(ConfigValidator, RejectsZeroOrdersPerSec)
{
    auto cfg = valid_config();
    cfg.risk.maxOrdersPerSec = 0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("maxOrdersPerSec"));
}

// ---------------------------------------------------------------------------
// Stop-loss
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsInvalidFixedPct)
{
    auto cfg = valid_config();
    cfg.risk.stop_loss.fixed_pct = 0.0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("fixed_pct"));
}

TEST(ConfigValidator, RejectsInvalidTrailingPct)
{
    auto cfg = valid_config();
    cfg.risk.stop_loss.trailing_pct = 0.6;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("trailing_pct"));
}

// ---------------------------------------------------------------------------
// Take-profit
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsTakeProfitSizeMismatch)
{
    auto cfg = valid_config();
    cfg.risk.take_profit.targets_pct = {0.005, 0.01};
    cfg.risk.take_profit.fractions = {0.5};
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("same length"));
}

TEST(ConfigValidator, RejectsTakeProfitFractionSumOverOne)
{
    auto cfg = valid_config();
    cfg.risk.take_profit.fractions = {0.5, 0.5, 0.5};
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("sum"));
}

// ---------------------------------------------------------------------------
// Strategy instances
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsEmptyStrategyList)
{
    auto cfg = valid_config();
    cfg.strategy.strategies.clear();
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("strategy"));
}

TEST(ConfigValidator, RejectsStrategyWithEmptyName)
{
    auto cfg = valid_config();
    cfg.strategy.strategies[0].name = "";
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("name"));
}

TEST(ConfigValidator, RejectsStrategySymbolNotInList)
{
    auto cfg = valid_config();
    cfg.strategy.strategies[0].symbol = "DOGE_USDT";
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("DOGE_USDT"));
}

TEST(ConfigValidator, RejectsNegativeOrderQuantity)
{
    auto cfg = valid_config();
    cfg.strategy.strategies[0].order_quantity = -0.001;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("order_quantity"));
}

TEST(ConfigValidator, RejectsConfidenceOutOfRange)
{
    {
        auto cfg = valid_config();
        cfg.strategy.strategies[0].min_confidence = -0.1;
        auto err = validateConfig(cfg);
        EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    }
    {
        auto cfg = valid_config();
        cfg.strategy.strategies[0].min_confidence = 1.1;
        auto err = validateConfig(cfg);
        EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    }
}

TEST(ConfigValidator, RejectsInvalidAggregatorThreshold)
{
    auto cfg = valid_config();
    cfg.strategy.signal_aggregator_threshold = 1.5;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos,
              err.message.find("signal_aggregator_threshold"));
}

// ---------------------------------------------------------------------------
// AI config
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsInvalidAiBackend)
{
    auto cfg = valid_config();
    cfg.ai.heartbeatIntervalSec = 300; // AI enabled.
    cfg.ai.backend = "gemini";
    cfg.ai.model = "some-model";
    cfg.ai.apiKey = "some-key";
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("backend"));
}

TEST(ConfigValidator, SkipsAiValidationWhenDisabled)
{
    auto cfg = valid_config();
    cfg.ai.heartbeatIntervalSec = 0; // AI disabled.
    cfg.ai.backend = "invalid";      // Should not be checked.
    cfg.ai.model = "";
    cfg.ai.apiKey = "";
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::Ok, err.code) << err.message;
}

// ---------------------------------------------------------------------------
// Log level
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsInvalidLogLevel)
{
    auto cfg = valid_config();
    cfg.log.level = "verbose";
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("log.level"));
}

TEST(ConfigValidator, AcceptsAllValidLogLevels)
{
    for (const auto &level :
         {"trace", "debug", "info", "warn", "error", "critical", "off"})
    {
        auto cfg = valid_config();
        cfg.log.level = level;
        auto err = validateConfig(cfg);
        EXPECT_EQ(ErrorCode::Ok, err.code)
            << "log level '" << level << "' should be valid";
    }
}

// ---------------------------------------------------------------------------
// Futures-specific validation tests
// ---------------------------------------------------------------------------

TEST(ConfigValidator, AcceptsFuturesStrategyWithLeverage)
{
    auto cfg = valid_config();
    cfg.strategy.strategies[0].market_type = MarketType::Futures;
    cfg.strategy.strategies[0].leverage = 5.0;
    cfg.risk.max_leverage = 10.0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::Ok, err.code);
}

TEST(ConfigValidator, RejectsLeverageBelowOne)
{
    auto cfg = valid_config();
    cfg.strategy.strategies[0].leverage = 0.5;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("leverage"));
}

TEST(ConfigValidator, RejectsLeverageExceedingMaxLeverage)
{
    auto cfg = valid_config();
    cfg.strategy.strategies[0].leverage = 20.0;
    cfg.risk.max_leverage = 10.0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("exceeds"));
}

TEST(ConfigValidator, RejectsMaxLeverageOutOfRange)
{
    auto cfg = valid_config();
    cfg.risk.max_leverage = 200.0; // > 125.0
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("max_leverage"));
}

TEST(ConfigValidator, RejectsMaxMarginUsedOutOfRange)
{
    auto cfg = valid_config();
    cfg.risk.max_margin_used = 1.5; // > 1.0
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("max_margin_used"));
}

TEST(ConfigValidator, AcceptsDefaultLeverageOne)
{
    auto cfg = valid_config();
    // Default leverage is 1.0 — should pass for both spot and futures
    EXPECT_DOUBLE_EQ(1.0, cfg.strategy.strategies[0].leverage);
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::Ok, err.code);
}

TEST(ConfigValidator, AcceptsMaxLeverageBoundary125)
{
    auto cfg = valid_config();
    cfg.risk.max_leverage = 125.0; // boundary value
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::Ok, err.code);
}

// ---------------------------------------------------------------------------
// Testnet validation tests
// ---------------------------------------------------------------------------

TEST(ConfigValidator, RejectsTestnetWithSpotStrategy)
{
    auto cfg = valid_config();
    cfg.exchange.testnet = true;
    cfg.strategy.strategies[0].market_type = MarketType::Spot;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find("testnet"));
}

TEST(ConfigValidator, AcceptsTestnetWithFuturesStrategy)
{
    auto cfg = valid_config();
    cfg.exchange.testnet = true;
    cfg.strategy.strategies[0].market_type = MarketType::Futures;
    cfg.strategy.strategies[0].leverage = 5.0;
    cfg.risk.max_leverage = 10.0;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::Ok, err.code) << err.message;
}

TEST(ConfigValidator, AcceptsMainnetWithSpotStrategy)
{
    auto cfg = valid_config();
    cfg.exchange.testnet = false;
    cfg.strategy.strategies[0].market_type = MarketType::Spot;
    auto err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::Ok, err.code);
}

} // namespace
} // namespace pulse
