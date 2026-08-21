// test_config_loader.cpp — Unit tests for TOML configuration loader
//
// Tests cover:
//   - Environment variable resolution (from_env: prefix)
//   - Individual section parsers
//   - Full integration (load a complete TOML file)
//   - Error handling (file not found, parse error, missing env var)

#include "core/config_loader.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <string>

// Cross-platform environment variable helpers (POSIX has setenv/unsetenv;
// Windows uses _putenv_s / _dupenv_s).
#ifdef _WIN32
static void setenv(const char *name, const char *value, int /*overwrite*/)
{
    _putenv_s(name, value);
}
static void unsetenv(const char *name)
{
    _putenv_s(name, "");
}
#endif

namespace pulse
{
namespace
{

// ---------------------------------------------------------------------------
// Helper: write a string to a temporary TOML file
// ---------------------------------------------------------------------------
class TempToml
{
  public:
    explicit TempToml(const std::string &content)
        : path_(std::filesystem::temp_directory_path()
                / ("pulsetrader_test_" + std::to_string(::getpid())
                   + "_" + std::to_string(counter_++) + ".toml"))
    {
        std::ofstream ofs(path_);
        ofs << content;
    }

    ~TempToml()
    {
        std::filesystem::remove(path_);
    }

    const std::filesystem::path &path() const
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

// Helper: build a valid minimal config for validation tests.
std::string minimal_toml()
{
    return R"(
symbols = ["BTC_USDT"]

[exchange]
apiKey = "test_key"
apiSecret = "test_secret"

[[strategy.instances]]
name = "momentum_scalper"
symbol = "BTC_USDT"
order_quantity = 0.001
min_confidence = 0.6
)";
}

// ---------------------------------------------------------------------------
// Error handling tests
// ---------------------------------------------------------------------------

TEST(ConfigLoader, FileNotFound)
{
    auto result = loadConfigFile("/nonexistent/path/config.toml");
    EXPECT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ConfigFileNotFound, error(result).code);
}

TEST(ConfigLoader, InvalidToml)
{
    TempToml tmp("this is not valid TOML = = = [[[");
    auto result = loadConfigFile(tmp.path());
    EXPECT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ConfigParseError, error(result).code);
}

// ---------------------------------------------------------------------------
// Environment variable resolution tests
// ---------------------------------------------------------------------------

TEST(ConfigLoader, ResolveEnvVar_ReplacesFromEnvPrefix)
{
    setenv("PULSE_TEST_KEY", "my_secret_value", 1);

    TempToml tmp(R"(
symbols = ["BTC_USDT"]

[exchange]
apiKey = "from_env:PULSE_TEST_KEY"
apiSecret = "test_secret"

[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("my_secret_value", value(result).exchange.apiKey);

    unsetenv("PULSE_TEST_KEY");
}

TEST(ConfigLoader, ResolveEnvVar_UnsetVarResolvesToEmpty)
{
    unsetenv("PULSE_NONEXISTENT_VAR");

    TempToml tmp(R"(
[exchange]
apiKey = "from_env:PULSE_NONEXISTENT_VAR"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("", value(result).exchange.apiKey);
}

TEST(ConfigLoader, ResolveEnvVar_EmptyVarResolvesToEmpty)
{
    setenv("PULSE_EMPTY_VAR", "", 1);

    TempToml tmp(R"(
[exchange]
apiKey = "from_env:PULSE_EMPTY_VAR"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("", value(result).exchange.apiKey);

    unsetenv("PULSE_EMPTY_VAR");
}

TEST(ConfigLoader, ResolveEnvVar_LeavesNonEnvStringsUntouched)
{
    TempToml tmp(R"(
symbols = ["BTC_USDT"]

[exchange]
apiKey = "plain_value"
apiSecret = "another_plain"
restBaseUrl = "https://api.example.com"

[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("plain_value", value(result).exchange.apiKey);
    EXPECT_EQ("another_plain", value(result).exchange.apiSecret);
    EXPECT_EQ("https://api.example.com", value(result).exchange.restBaseUrl);
}

TEST(ConfigLoader, ResolveEnvVar_ResolvesNestedTables)
{
    setenv("PULSE_NESTED_VAL", "nested_secret", 1);

    TempToml tmp(R"(
symbols = ["BTC_USDT"]

[exchange]
apiKey = "key"
apiSecret = "secret"

[ai]
apiKey = "from_env:PULSE_NESTED_VAL"

[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("nested_secret", value(result).ai.apiKey);

    unsetenv("PULSE_NESTED_VAL");
}

// ---------------------------------------------------------------------------
// Section parser tests
// ---------------------------------------------------------------------------

TEST(ConfigLoader, ParseExchange_FullTable)
{
    TempToml tmp(R"(
[exchange]
apiKey = "mykey"
apiSecret = "mysecret"
restBaseUrl = "https://custom.api.com"
wsUrl = "wss://custom.ws.com"
proxyUrl = "http://proxy:8080"
restTimeoutMs = 5000
maxRetries = 5
wsReconnectBaseMs = 2000
wsReconnectMaxMs = 60000
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &ex = value(result).exchange;
    EXPECT_EQ("mykey", ex.apiKey);
    EXPECT_EQ("mysecret", ex.apiSecret);
    EXPECT_EQ("https://custom.api.com", ex.restBaseUrl);
    EXPECT_EQ("wss://custom.ws.com", ex.wsUrl);
    EXPECT_EQ("http://proxy:8080", ex.proxyUrl);
    EXPECT_EQ(5000u, ex.restTimeoutMs);
    EXPECT_EQ(5u, ex.maxRetries);
    EXPECT_EQ(2000u, ex.wsReconnectBaseMs);
    EXPECT_EQ(60000u, ex.wsReconnectMaxMs);
}

TEST(ConfigLoader, ParseExchange_PartialTable)
{
    TempToml tmp(R"(
[exchange]
apiKey = "partial_key"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &ex = value(result).exchange;
    EXPECT_EQ("partial_key", ex.apiKey);
    // Defaults preserved.
    EXPECT_EQ("https://api.gateio.ws", ex.restBaseUrl);
    EXPECT_EQ(10000u, ex.restTimeoutMs);
    EXPECT_EQ(3u, ex.maxRetries);
}

TEST(ConfigLoader, ParseLog_AllFields)
{
    TempToml tmp(R"(
[log]
level = "debug"
logDir = "/var/log/pulse"
toConsole = false
toFile = true
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &lg = value(result).log;
    EXPECT_EQ("debug", lg.level);
    EXPECT_EQ("/var/log/pulse", lg.logDir);
    EXPECT_FALSE(lg.toConsole);
    EXPECT_TRUE(lg.toFile);
}

TEST(ConfigLoader, ParseSymbols_Array)
{
    TempToml tmp(R"(
symbols = ["BTC_USDT", "ETH_USDT", "SOL_USDT"]
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &syms = value(result).symbols;
    ASSERT_EQ(3u, syms.size());
    EXPECT_EQ("BTC_USDT", syms[0]);
    EXPECT_EQ("ETH_USDT", syms[1]);
    EXPECT_EQ("SOL_USDT", syms[2]);
}

TEST(ConfigLoader, ParseAi_WithEnvApiKey)
{
    setenv("PULSE_AI_KEY", "ai_secret_123", 1);

    TempToml tmp(R"(
[ai]
backend = "openai"
model = "gpt-4o"
apiKey = "from_env:PULSE_AI_KEY"
heartbeatIntervalSec = 600
requestTimeoutMs = 60000
maxRetries = 3
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &ai = value(result).ai;
    EXPECT_EQ("openai", ai.backend);
    EXPECT_EQ("gpt-4o", ai.model);
    EXPECT_EQ("ai_secret_123", ai.apiKey);
    EXPECT_EQ(600u, ai.heartbeatIntervalSec);
    EXPECT_EQ(60000u, ai.requestTimeoutMs);
    EXPECT_EQ(3u, ai.maxRetries);

    unsetenv("PULSE_AI_KEY");
}

TEST(ConfigLoader, ParseRisk_WithNestedStopLoss)
{
    TempToml tmp(R"(
[risk]
maxPositionNotional = 2000.0
maxOpenPositions = 10
maxDailyDrawdown = 0.03
maxDrawdown = 0.1
maxOrdersPerSec = 8
maxSymbolNotional = 800.0

[risk.stop_loss]
mode = "Fixed"
fixed_pct = 0.02
trailing_pct = 0.01
max_hold_seconds = 600
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &risk = value(result).risk;
    EXPECT_DOUBLE_EQ(2000.0, risk.maxPositionNotional);
    EXPECT_EQ(10, risk.maxOpenPositions);
    EXPECT_DOUBLE_EQ(0.03, risk.maxDailyDrawdown);
    EXPECT_DOUBLE_EQ(0.1, risk.maxDrawdown);
    EXPECT_EQ(8u, risk.maxOrdersPerSec);
    EXPECT_DOUBLE_EQ(800.0, risk.maxSymbolNotional);

    EXPECT_EQ(StopMode::Fixed, risk.stop_loss.mode);
    EXPECT_DOUBLE_EQ(0.02, risk.stop_loss.fixed_pct);
    EXPECT_DOUBLE_EQ(0.01, risk.stop_loss.trailing_pct);
    EXPECT_EQ(600u, risk.stop_loss.max_hold_seconds);
}

TEST(ConfigLoader, ParseRisk_WithPerMarketNotionalCaps)
{
    TempToml tmp(R"(
[risk]
maxPositionNotional = 1000.0
maxPositionNotionalFutures = 6000.0
maxPositionNotionalCfd = 4000.0
maxPositionNotionalSpot = 2000.0
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &risk = value(result).risk;
    EXPECT_DOUBLE_EQ(1000.0, risk.maxPositionNotional);
    ASSERT_TRUE(risk.maxPositionNotionalFutures.has_value());
    EXPECT_DOUBLE_EQ(6000.0, risk.maxPositionNotionalFutures.value());
    ASSERT_TRUE(risk.maxPositionNotionalCfd.has_value());
    EXPECT_DOUBLE_EQ(4000.0, risk.maxPositionNotionalCfd.value());
    ASSERT_TRUE(risk.maxPositionNotionalSpot.has_value());
    EXPECT_DOUBLE_EQ(2000.0, risk.maxPositionNotionalSpot.value());
}

TEST(ConfigLoader, ParseRisk_PerMarketCapsOptional)
{
    // Absent keys stay std::nullopt (fallback to maxPositionNotional).
    TempToml tmp(R"(
[risk]
maxPositionNotional = 1000.0
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &risk = value(result).risk;
    EXPECT_FALSE(risk.maxPositionNotionalFutures.has_value());
    EXPECT_FALSE(risk.maxPositionNotionalCfd.has_value());
    EXPECT_FALSE(risk.maxPositionNotionalSpot.has_value());
    EXPECT_FALSE(risk.minAvailableAfterStopUsd.has_value());
}

TEST(ConfigLoader, ParseRisk_MinAvailableAfterStopUsd)
{
    TempToml tmp(R"(
[risk]
minAvailableAfterStopUsd = 6.0
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &risk = value(result).risk;
    ASSERT_TRUE(risk.minAvailableAfterStopUsd.has_value());
    EXPECT_DOUBLE_EQ(6.0, risk.minAvailableAfterStopUsd.value());
}

TEST(ConfigLoader, ParseRisk_WithTakeProfit)
{
    TempToml tmp(R"(
[risk.take_profit]
enabled = true
targets_pct = [0.003, 0.006, 0.015]
fractions = [0.25, 0.25, 0.50]
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &tp = value(result).risk.take_profit;
    EXPECT_TRUE(tp.enabled);
    ASSERT_EQ(3u, tp.targets_pct.size());
    EXPECT_DOUBLE_EQ(0.003, tp.targets_pct[0]);
    EXPECT_DOUBLE_EQ(0.006, tp.targets_pct[1]);
    EXPECT_DOUBLE_EQ(0.015, tp.targets_pct[2]);
    ASSERT_EQ(3u, tp.fractions.size());
    EXPECT_DOUBLE_EQ(0.25, tp.fractions[0]);
    EXPECT_DOUBLE_EQ(0.50, tp.fractions[2]);
}

TEST(ConfigLoader, ParseStrategy_MultipleInstances)
{
    TempToml tmp(R"(
[strategy]
signal_aggregator_threshold = 0.8
signal_cooldown_sec = 60

[[strategy.instances]]
name = "momentum_scalper"
symbol = "BTC_USDT"
order_quantity = 0.002
min_confidence = 0.7
poll_interval_ms = 100

[[strategy.instances]]
name = "orderbook_scalper"
symbol = "BTC_USDT"
order_quantity = 0.001
min_confidence = 0.65
poll_interval_ms = 50

[[strategy.instances]]
name = "mean_reversion_scalper"
symbol = "ETH_USDT"
order_quantity = 0.01
min_confidence = 0.6
enabled = false
poll_interval_ms = 500
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &strat = value(result).strategy;
    EXPECT_DOUBLE_EQ(0.8, strat.signal_aggregator_threshold);
    EXPECT_EQ(60u, strat.signal_cooldown_sec);
    ASSERT_EQ(3u, strat.strategies.size());

    EXPECT_EQ("momentum_scalper", strat.strategies[0].name);
    EXPECT_DOUBLE_EQ(0.002, strat.strategies[0].order_quantity);
    EXPECT_EQ(100u, strat.strategies[0].poll_interval_ms);

    EXPECT_EQ("orderbook_scalper", strat.strategies[1].name);
    EXPECT_EQ(50u, strat.strategies[1].poll_interval_ms);

    EXPECT_EQ("mean_reversion_scalper", strat.strategies[2].name);
    EXPECT_FALSE(strat.strategies[2].enabled);
}

TEST(ConfigLoader, ParseStrategy_SignalOnly)
{
    // Explicit true.
    TempToml enabled_toml(R"(
[strategy]
signal_only = true
)");
    auto enabled = loadConfigFile(enabled_toml.path());
    ASSERT_TRUE(ok(enabled)) << error(enabled).message;
    EXPECT_TRUE(value(enabled).strategy.signal_only);

    // Omitted → default false (auto-trading behavior unchanged).
    TempToml default_toml(R"(
[strategy]
signal_aggregator_threshold = 0.7
)");
    auto dflt = loadConfigFile(default_toml.path());
    ASSERT_TRUE(ok(dflt)) << error(dflt).message;
    EXPECT_FALSE(value(dflt).strategy.signal_only);

    // Explicit false.
    TempToml disabled_toml(R"(
[strategy]
signal_only = false
)");
    auto disabled = loadConfigFile(disabled_toml.path());
    ASSERT_TRUE(ok(disabled)) << error(disabled).message;
    EXPECT_FALSE(value(disabled).strategy.signal_only);
}

TEST(ConfigLoader, ParseControl_AllFields)
{
    TempToml tmp(R"(
[control]
enabled = true
bindAddress = "0.0.0.0"
port = 9091
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &ctrl = value(result).control;
    EXPECT_TRUE(ctrl.enabled);
    EXPECT_EQ("0.0.0.0", ctrl.bindAddress);
    EXPECT_EQ(9091, ctrl.port);
}

TEST(ConfigLoader, ParseTwitter_AndNews)
{
    TempToml tmp(R"(
[twitter]
enabled = true
bearerToken = "tw_token"
keywords = ["bitcoin", "crypto"]
maxTweets = 50
pollIntervalSec = 120

[news]
enabled = true
apiKey = "news_key"
provider = "cryptopanic"
keywords = ["regulation"]
maxArticles = 30
pollIntervalSec = 600
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &tw = value(result).twitter;
    EXPECT_TRUE(tw.enabled);
    EXPECT_EQ("tw_token", tw.bearerToken);
    ASSERT_EQ(2u, tw.keywords.size());
    EXPECT_EQ("bitcoin", tw.keywords[0]);
    EXPECT_EQ(50u, tw.maxTweets);
    EXPECT_EQ(120u, tw.pollIntervalSec);

    const auto &nw = value(result).news;
    EXPECT_TRUE(nw.enabled);
    EXPECT_EQ("news_key", nw.apiKey);
    EXPECT_EQ("cryptopanic", nw.provider);
    ASSERT_EQ(1u, nw.keywords.size());
    EXPECT_EQ(30u, nw.maxArticles);
}

// ---------------------------------------------------------------------------
// Integration tests
// ---------------------------------------------------------------------------

TEST(ConfigLoader, LoadConfig_ValidCompleteFile)
{
    TempToml tmp(minimal_toml());

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &cfg = value(result);
    EXPECT_EQ("test_key", cfg.exchange.apiKey);
    EXPECT_EQ("test_secret", cfg.exchange.apiSecret);
    ASSERT_EQ(1u, cfg.symbols.size());
    EXPECT_EQ("BTC_USDT", cfg.symbols[0]);
    ASSERT_EQ(1u, cfg.strategy.strategies.size());
    EXPECT_EQ("momentum_scalper", cfg.strategy.strategies[0].name);
}

TEST(ConfigLoader, LoadConfig_MinimalFile_DefaultsPreserved)
{
    TempToml tmp(R"(
symbols = ["ETH_USDT"]

[exchange]
apiKey = "k"
apiSecret = "s"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &cfg = value(result);
    // Exchange defaults preserved for unset fields.
    EXPECT_EQ("https://api.gateio.ws", cfg.exchange.restBaseUrl);
    EXPECT_EQ(10000u, cfg.exchange.restTimeoutMs);
    // Log defaults preserved.
    EXPECT_EQ("info", cfg.log.level);
    EXPECT_TRUE(cfg.log.toConsole);
    // Risk defaults preserved.
    EXPECT_DOUBLE_EQ(1000.0, cfg.risk.maxPositionNotional);
    EXPECT_EQ(5, cfg.risk.maxOpenPositions);
    // Per-market notional overrides default to unset (fallback semantics).
    EXPECT_FALSE(cfg.risk.maxPositionNotionalFutures.has_value());
    EXPECT_FALSE(cfg.risk.maxPositionNotionalCfd.has_value());
    EXPECT_FALSE(cfg.risk.maxPositionNotionalSpot.has_value());
    // Control defaults preserved.
    EXPECT_TRUE(cfg.control.enabled);
    EXPECT_EQ(8081, cfg.control.port);
    // AI defaults preserved.
    EXPECT_EQ("claude", cfg.ai.backend);
}

TEST(ConfigLoader, LoadConfig_UnknownKeysIgnored)
{
    TempToml tmp(R"(
symbols = ["BTC_USDT"]

[exchange]
apiKey = "k"
apiSecret = "s"
futureField = "ignored"

[future_section]
key = "value"

[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("k", value(result).exchange.apiKey);
}

TEST(ConfigLoader, LoadConfig_EnvVarEndToEnd)
{
    setenv("PULSE_E2E_KEY", "e2e_key_value", 1);
    setenv("PULSE_E2E_SECRET", "e2e_secret_value", 1);

    TempToml tmp(R"(
symbols = ["BTC_USDT"]

[exchange]
apiKey = "from_env:PULSE_E2E_KEY"
apiSecret = "from_env:PULSE_E2E_SECRET"

[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("e2e_key_value", value(result).exchange.apiKey);
    EXPECT_EQ("e2e_secret_value", value(result).exchange.apiSecret);

    unsetenv("PULSE_E2E_KEY");
    unsetenv("PULSE_E2E_SECRET");
}

TEST(ConfigLoader, ParseStopMode_InvalidValue)
{
    TempToml tmp(R"(
[risk.stop_loss]
mode = "InvalidMode"
)");

    auto result = loadConfigFile(tmp.path());
    EXPECT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ConfigInvalidValue, error(result).code);
}

// ---------------------------------------------------------------------------
// Futures-specific config tests
// ---------------------------------------------------------------------------

TEST(ConfigLoader, ParseExchange_FuturesWsUrl)
{
    TempToml tmp(R"(
[exchange]
futuresWsUrl = "wss://custom-futures-ws.example.com/v4/ws/usdt"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("wss://custom-futures-ws.example.com/v4/ws/usdt",
              value(result).exchange.futuresWsUrl);
}

TEST(ConfigLoader, ParseExchange_FuturesWsUrlDefault)
{
    TempToml tmp(R"(
[exchange]
apiKey = "k"
apiSecret = "s"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ("wss://fx-ws.gateio.ws/v4/ws/usdt",
              value(result).exchange.futuresWsUrl);
}

TEST(ConfigLoader, ParseRisk_MaxLeverageAndMargin)
{
    TempToml tmp(R"(
[risk]
max_leverage = 20.0
max_margin_used = 0.3
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_DOUBLE_EQ(20.0, value(result).risk.max_leverage);
    EXPECT_DOUBLE_EQ(0.3, value(result).risk.max_margin_used);
}

TEST(ConfigLoader, ParseStrategyInstance_FuturesFields)
{
    TempToml tmp(R"(
[[strategy.instances]]
name = "momentum_scalper"
symbol = "BTC_USDT"
market_type = "futures"
leverage = 10
margin_mode = "isolated"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    ASSERT_EQ(1u, value(result).strategy.strategies.size());

    const auto &inst = value(result).strategy.strategies[0];
    EXPECT_EQ(MarketType::Futures, inst.market_type);
    EXPECT_DOUBLE_EQ(10.0, inst.leverage);
    EXPECT_EQ(MarginMode::Isolated, inst.margin_mode);
}

TEST(ConfigLoader, ParseStrategyInstance_InvalidMarketType)
{
    TempToml tmp(R"(
[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
market_type = "options"
)");

    auto result = loadConfigFile(tmp.path());
    EXPECT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ConfigInvalidValue, error(result).code);
}

// ---------------------------------------------------------------------------
// Testnet config tests
// ---------------------------------------------------------------------------

TEST(ConfigLoader, ParseExchange_TestnetTrue)
{
    TempToml tmp(R"(
[exchange]
apiKey = "testnet_key"
apiSecret = "testnet_secret"
testnet = true
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_TRUE(value(result).exchange.testnet);
    EXPECT_EQ("testnet_key", value(result).exchange.apiKey);
}

TEST(ConfigLoader, ParseExchange_TestnetFalse)
{
    TempToml tmp(R"(
[exchange]
apiKey = "k"
apiSecret = "s"
testnet = false
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_FALSE(value(result).exchange.testnet);
}

TEST(ConfigLoader, ParseExchange_TestnetDefaultFalse)
{
    TempToml tmp(R"(
[exchange]
apiKey = "k"
apiSecret = "s"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_FALSE(value(result).exchange.testnet);
}

// ---------------------------------------------------------------------------
// Testnet URL auto-switching tests
// ---------------------------------------------------------------------------

TEST(ConfigLoader, TestnetAutoSwitch_AllUrlsSwitchToTestnet)
{
    // testnet=true with NO explicit URLs → all URLs should be testnet defaults.
    TempToml tmp(R"(
[exchange]
apiKey = "k"
apiSecret = "s"
testnet = true
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    const auto &ex = value(result).exchange;
    EXPECT_TRUE(ex.testnet);
    EXPECT_EQ(url::kTestnetRest, ex.restBaseUrl);
    EXPECT_EQ(url::kTestnetSpotWs, ex.wsUrl);
    EXPECT_EQ(url::kTestnetFuturesWs, ex.futuresWsUrl);
}

TEST(ConfigLoader, TestnetExplicitOverride_KeepsUserUrl)
{
    // testnet=true WITH explicit futuresWsUrl → user's URL is preserved.
    // (e.g. China user falling back to mainnet WS for market data)
    TempToml tmp(R"(
[exchange]
apiKey = "k"
apiSecret = "s"
testnet = true
futuresWsUrl = "wss://fx-ws.gateio.ws/v4/ws/usdt"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    const auto &ex = value(result).exchange;
    EXPECT_TRUE(ex.testnet);
    // REST and spot WS auto-switched to testnet.
    EXPECT_EQ(url::kTestnetRest, ex.restBaseUrl);
    EXPECT_EQ(url::kTestnetSpotWs, ex.wsUrl);
    // But futuresWsUrl was explicitly set to mainnet → preserved.
    EXPECT_EQ("wss://fx-ws.gateio.ws/v4/ws/usdt", ex.futuresWsUrl);
}

TEST(ConfigLoader, MainnetDefault_AllUrlsAreMainnet)
{
    // testnet=false (default) with NO explicit URLs → all URLs are mainnet.
    TempToml tmp(R"(
[exchange]
apiKey = "k"
apiSecret = "s"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    const auto &ex = value(result).exchange;
    EXPECT_FALSE(ex.testnet);
    EXPECT_EQ(url::kMainnetRest, ex.restBaseUrl);
    EXPECT_EQ(url::kMainnetSpotWs, ex.wsUrl);
    EXPECT_EQ(url::kMainnetFuturesWs, ex.futuresWsUrl);
}

TEST(ConfigLoader, MainnetExplicit_UsesUserUrls)
{
    // testnet=false WITH explicit URLs → user's URLs are used.
    TempToml tmp(R"(
[exchange]
apiKey = "k"
apiSecret = "s"
testnet = false
restBaseUrl = "https://custom-rest.example.com"
wsUrl = "wss://custom-ws.example.com/ws/"
futuresWsUrl = "wss://custom-fx.example.com/ws/usdt"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    const auto &ex = value(result).exchange;
    EXPECT_FALSE(ex.testnet);
    EXPECT_EQ("https://custom-rest.example.com", ex.restBaseUrl);
    EXPECT_EQ("wss://custom-ws.example.com/ws/", ex.wsUrl);
    EXPECT_EQ("wss://custom-fx.example.com/ws/usdt", ex.futuresWsUrl);
}

// ---------------------------------------------------------------------------
// order_type / maker_timeout_ms parsing
// ---------------------------------------------------------------------------

TEST(ConfigLoader, ParseStrategyInstance_OrderTypeAndMakerTimeout)
{
    TempToml tmp(R"(
[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
order_type = "maker_first"
maker_timeout_ms = 800
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    const auto &inst = value(result).strategy.strategies[0];
    EXPECT_EQ(OrderType::MakerFirst, inst.order_type);
    EXPECT_EQ(800u, inst.maker_timeout_ms);
}

TEST(ConfigLoader, ParseStrategyInstance_RejectsUnknownOrderType)
{
    TempToml tmp(R"(
[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
order_type = "taker"
)");

    auto result = loadConfigFile(tmp.path());
    EXPECT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ConfigInvalidValue, error(result).code);
    EXPECT_NE(std::string::npos, error(result).message.find("order_type"));
}

TEST(ConfigLoader, ParseStrategyInstance_DefaultsOrderTypeToMarket)
{
    TempToml tmp(R"(
[[strategy.instances]]
name = "test"
symbol = "BTC_USDT"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    const auto &inst = value(result).strategy.strategies[0];
    EXPECT_EQ(OrderType::Market, inst.order_type);
    EXPECT_EQ(0u, inst.maker_timeout_ms);
}

TEST(ConfigLoader, ParseSqlite_RecordMarketDefaultFalse)
{
    TempToml tmp(R"(
[sqlite]
enabled = true
dbPath = "data/trades.db"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    const auto &sql = value(result).sqlite;
    EXPECT_TRUE(sql.enabled);
    EXPECT_EQ("data/trades.db", sql.dbPath);
    EXPECT_FALSE(sql.recordMarketData);
}

TEST(ConfigLoader, ParseSqlite_RecordMarketParsed)
{
    TempToml tmp(R"(
[sqlite]
enabled = true
dbPath = "data/trades.db"
record_market = true
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    const auto &sql = value(result).sqlite;
    EXPECT_TRUE(sql.enabled);
    EXPECT_TRUE(sql.recordMarketData);
}

// ---------------------------------------------------------------------------
// custom_params — coin-specific instance parameter channel (M26)
// ---------------------------------------------------------------------------

TEST(ConfigLoader, ParseStrategyInstance_CustomParamsInlineTable)
{
    // Both float and integer values are accepted (int → double).
    TempToml tmp(R"(
[[strategy.instances]]
name = "eth_scalper"
symbol = "ETH_USDT"
custom_params = { eth_atr_step = 0.05, eth_spike_filter_usd = 120, eth_scale = 1.0 }
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    ASSERT_EQ(1u, value(result).strategy.strategies.size());

    const auto &inst = value(result).strategy.strategies[0];
    ASSERT_EQ(3u, inst.custom_params.size());
    EXPECT_DOUBLE_EQ(0.05, inst.custom_params.at("eth_atr_step"));
    EXPECT_DOUBLE_EQ(120.0, inst.custom_params.at("eth_spike_filter_usd"));
    EXPECT_DOUBLE_EQ(1.0, inst.custom_params.at("eth_scale"));
}

TEST(ConfigLoader, ParseStrategyInstance_CustomParamsAbsent)
{
    // Legacy instances without custom_params → empty map (backward compatible).
    TempToml tmp(R"(
[[strategy.instances]]
name = "momentum_scalper"
symbol = "BTC_USDT"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;
    ASSERT_EQ(1u, value(result).strategy.strategies.size());
    EXPECT_TRUE(value(result).strategy.strategies[0].custom_params.empty());
}

TEST(ConfigLoader, ParseStrategyInstance_CustomParamsNonNumberRejected)
{
    TempToml tmp(R"(
[[strategy.instances]]
name = "eth_scalper"
symbol = "ETH_USDT"
custom_params = { eth_atr_step = "abc" }
)");

    auto result = loadConfigFile(tmp.path());
    EXPECT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ConfigInvalidValue, error(result).code);
}

TEST(ConfigLoader, ParseStrategyInstance_CustomParamsNonTableRejected)
{
    TempToml tmp(R"(
[[strategy.instances]]
name = "eth_scalper"
symbol = "ETH_USDT"
custom_params = 5
)");

    auto result = loadConfigFile(tmp.path());
    EXPECT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ConfigInvalidValue, error(result).code);
}

TEST(ConfigLoader, ParseGrid_AllFields)
{
    TempToml tmp(R"(
[grid]
enabled = true
symbol = "ETH_USDT"
market_type = "futures"
levels = 12
qty_per_level = 0.02
step_mode = "atr"
step_fixed = 5.0
step_atr_mult = 0.5
step_min = 3.0
step_max = 8.0
atr_period = 40
tp_distance_steps = 2.0
anchor_offset_steps = 1.0
lower_reanchor_steps = 3.0
upper_reanchor_steps = 1.0
protect_line_a_steps = 2.0
protect_line_b_usd = -30.0
daily_loss_limit_usd = -10.0
daily_reset_hour = 8
reanchor_cooldown_min = 30
spike_freeze_min = 30
spike_pct = 0.01
spike_atr_mult = 3.0
state_file = "data/grid_state_ETH_USDT.json"
force = true
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &g = value(result).grid;
    EXPECT_TRUE(g.enabled);
    EXPECT_EQ("ETH_USDT", g.symbol);
    EXPECT_EQ("futures", g.market_type);
    EXPECT_EQ(12, g.levels);
    EXPECT_DOUBLE_EQ(0.02, g.qty_per_level);
    EXPECT_EQ("atr", g.step_mode);
    EXPECT_DOUBLE_EQ(5.0, g.step_fixed);
    EXPECT_DOUBLE_EQ(0.5, g.step_atr_mult);
    EXPECT_DOUBLE_EQ(3.0, g.step_min);
    EXPECT_DOUBLE_EQ(8.0, g.step_max);
    EXPECT_EQ(40, g.atr_period);
    EXPECT_DOUBLE_EQ(2.0, g.tp_distance_steps);
    EXPECT_DOUBLE_EQ(1.0, g.anchor_offset_steps);
    EXPECT_DOUBLE_EQ(3.0, g.lower_reanchor_steps);
    EXPECT_DOUBLE_EQ(1.0, g.upper_reanchor_steps);
    EXPECT_DOUBLE_EQ(2.0, g.protect_line_a_steps);
    EXPECT_DOUBLE_EQ(-30.0, g.protect_line_b_usd);
    EXPECT_DOUBLE_EQ(-10.0, g.daily_loss_limit_usd);
    EXPECT_EQ(8, g.daily_reset_hour);
    EXPECT_EQ(30, g.reanchor_cooldown_min);
    EXPECT_EQ(30, g.spike_freeze_min);
    EXPECT_DOUBLE_EQ(0.01, g.spike_pct);
    EXPECT_DOUBLE_EQ(3.0, g.spike_atr_mult);
    EXPECT_EQ("data/grid_state_ETH_USDT.json", g.state_file);
    EXPECT_TRUE(g.force);
}

TEST(ConfigLoader, ParseGrid_AbsentSectionKeepsDefaults)
{
    TempToml tmp(R"(
[log]
level = "info"
)");

    auto result = loadConfigFile(tmp.path());
    ASSERT_TRUE(ok(result)) << error(result).message;

    const auto &g = value(result).grid;
    EXPECT_FALSE(g.enabled);
    EXPECT_EQ("ETH_USDT", g.symbol);
    EXPECT_EQ(12, g.levels);
}

} // namespace
} // namespace pulse
