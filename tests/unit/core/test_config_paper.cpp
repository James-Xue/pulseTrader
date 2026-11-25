// test_config_paper.cpp — Fail-closed rules for the [paper] configuration.
//
// These tests pin the properties that make a paper run safe:
//   1. paper REQUIRES empty credentials, live REQUIRES non-empty ones
//   2. paper and testnet are mutually exclusive
//   3. paper resources (port / database / log dir / lock) must be distinct
//   4. every wired CFD symbol must carry a contract size
//   5. an unknown key inside [paper] is a hard parse error
//
// A regression here is a real-money regression, not a formatting issue.

#include "core/config.hpp"
#include "core/config_loader.hpp"
#include "core/config_validator.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace pulse;

namespace
{

PulseConfig makePaperConfig()
{
    PulseConfig cfg;
    cfg.symbols = { "XAUUSD" };
    cfg.exchange.apiKey.clear();
    cfg.exchange.apiSecret.clear();
    cfg.exchange.testnet = false;

    cfg.paper.enabled = true;
    cfg.paper.account_id = "paper-test";
    cfg.paper.contract_volume["XAUUSD"] = 100.0;

    cfg.control.port = 8082;
    cfg.sqlite.dbPath = "data/trades-paper.db";
    cfg.log.logDir = "logs/paper";
    cfg.paper.lock_file = "data/engine.paper.lock";

    StrategyInstanceConfig inst;
    inst.name = "gold_test";
    inst.symbol = "XAUUSD";
    inst.market_type = MarketType::Cfd;
    inst.leverage = 10.0; // risk.max_leverage defaults to 10
    inst.enabled = true;
    cfg.strategy.strategies.push_back(inst);

    return cfg;
}

void expectValid(const PulseConfig &cfg)
{
    const PulseError err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::Ok, err.code) << err.message;
}

void expectInvalid(const PulseConfig &cfg, const std::string &needle)
{
    const PulseError err = validateConfig(cfg);
    EXPECT_EQ(ErrorCode::ConfigValidationError, err.code);
    EXPECT_NE(std::string::npos, err.message.find(needle))
        << "actual message: " << err.message;
}

} // namespace

TEST(ConfigPaperTest, ValidPaperConfigPasses)
{
    expectValid(makePaperConfig());
}

TEST(ConfigPaperTest, PaperRequiresEmptyCredentials)
{
    PulseConfig cfg = makePaperConfig();
    cfg.exchange.apiKey = "not-empty";
    expectInvalid(cfg, "empty exchange.apiKey");
}

TEST(ConfigPaperTest, LiveStillRequiresCredentials)
{
    PulseConfig cfg = makePaperConfig();
    cfg.paper.enabled = false;
    // Live config with no keys must fail exactly as before.
    expectInvalid(cfg, "exchange.apiKey must not be empty");
}

TEST(ConfigPaperTest, PaperAndTestnetAreMutuallyExclusive)
{
    PulseConfig cfg = makePaperConfig();
    cfg.exchange.testnet = true;
    expectInvalid(cfg, "mutually exclusive");
}

TEST(ConfigPaperTest, PaperMustNotShareTheLiveControlPort)
{
    PulseConfig cfg = makePaperConfig();
    cfg.control.port = 8081;
    expectInvalid(cfg, "control.port");
}

TEST(ConfigPaperTest, PaperMustNotShareTheLiveDatabase)
{
    PulseConfig cfg = makePaperConfig();
    cfg.sqlite.dbPath = "trades.db";
    expectInvalid(cfg, "sqlite.dbPath");
}

TEST(ConfigPaperTest, PaperMustNotShareTheLiveLogDirectory)
{
    PulseConfig cfg = makePaperConfig();
    cfg.log.logDir = "logs";
    expectInvalid(cfg, "log.logDir");
}

TEST(ConfigPaperTest, PaperMustNotShareTheLiveLock)
{
    PulseConfig cfg = makePaperConfig();
    cfg.paper.lock_file = "data/engine.lock";
    expectInvalid(cfg, "lock_file");
}

TEST(ConfigPaperTest, PaperRejectsGrid)
{
    PulseConfig cfg = makePaperConfig();
    cfg.grid.enabled = true;
    expectInvalid(cfg, "grid.enabled");
}

TEST(ConfigPaperTest, CfdSymbolNeedsContractVolume)
{
    PulseConfig cfg = makePaperConfig();
    cfg.paper.contract_volume.clear();
    expectInvalid(cfg, "contract_volume");
}

TEST(ConfigPaperTest, FeeRatesMustBeSane)
{
    PulseConfig cfg = makePaperConfig();
    cfg.paper.taker_fee_rate = 0.5; // 50% — almost certainly a typo
    expectInvalid(cfg, "fee rates");
}

TEST(ConfigPaperTest, FillRatioMustBeInRange)
{
    PulseConfig cfg = makePaperConfig();
    cfg.paper.fill_ratio = 0.0;
    expectInvalid(cfg, "fill_ratio");
}

TEST(ConfigPaperTest, UnknownKeyInsidePaperIsAHardParseError)
{
    const auto path = std::filesystem::temp_directory_path()
        / "pulse-paper-config-unknown-key.toml";
    {
        std::ofstream out(path, std::ios::trunc);
        // `enable` (missing 'd') is the exact typo the strict parser exists for.
        out << "[paper]\nenable = true\n";
    }

    const auto result = loadConfigFile(path.string());
    std::error_code ec;
    std::filesystem::remove(path, ec);

    ASSERT_FALSE(ok(result));
    EXPECT_NE(std::string::npos, error(result).message.find("unknown key"))
        << "actual message: " << error(result).message;
}
