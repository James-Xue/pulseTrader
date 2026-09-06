// test_quanto_resolver.cpp — QuantoResolver contract-metadata resolution.
//
// Offline by construction: every test pre-seeds the on-disk cache file so
// the resolver never touches the network (the REST fetch path is exercised
// end-to-end by the E2E verification, not by unit tests).

#include "backtest/QuantoResolver.hpp"

#include "core/PulseError.hpp"
#include "exchange/GateRestClient.hpp"
#include "market/SymbolRegistry.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace pulse::backtest::test
{

namespace
{

class QuantoResolverTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_cachePath = std::filesystem::temp_directory_path()
            / ("pulse_qr_" + std::string(::testing::UnitTest::GetInstance()
                                             ->current_test_info()
                                             ->name())
                   + ".json");
        std::filesystem::remove(m_cachePath);
    }

    void TearDown() override
    {
        std::filesystem::remove(m_cachePath);
        std::filesystem::remove(m_cachePath.string() + ".tmp");
    }

    /// Write a fresh (now-based timestamp) cache file with the given symbols.
    void writeFreshCache(
        const std::vector<std::pair<std::string, double>> &symbols)
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        nlohmann::json futures = nlohmann::json::object();
        for (const auto &[name, quanto] : symbols)
        {
            futures[name] = {
                { "quanto_multiplier", quanto },
                { "tick_size", 0.0001 },
                { "leverage_max", 125.0 },
                { "order_size_min", 1 },
                { "order_size_max", 1000000 },
            };
        }
        const nlohmann::json root = {
            { "fetched_at_ms", now - 1000 }, // fresh (12h TTL)
            { "futures", futures },
        };
        std::ofstream out(m_cachePath);
        out << root.dump(2);
    }

    std::filesystem::path m_cachePath;
};

} // anonymous namespace

TEST_F(QuantoResolverTest, Spot_ResolvesToOneWithoutNetwork)
{
    exchange::GateRestClient rest(ExchangeConfig{}, MarketType::Spot);
    QuantoResolver resolver(rest, m_cachePath.string());
    const auto result = resolver.resolveQuanto("ETH_USDT", MarketType::Spot);
    ASSERT_TRUE(ok(result));
    EXPECT_DOUBLE_EQ(1.0, value(result));
}

TEST_F(QuantoResolverTest, Cfd_IsAnError)
{
    exchange::GateRestClient rest(ExchangeConfig{}, MarketType::Cfd);
    QuantoResolver resolver(rest, m_cachePath.string());
    const auto result = resolver.resolveQuanto("XAUUSD", MarketType::Cfd);
    ASSERT_FALSE(ok(result));
    EXPECT_NE(std::string::npos, error(result).message.find("CFD"));
}

TEST_F(QuantoResolverTest, CacheHit_ResolvesKnownSymbol)
{
    writeFreshCache({ { "DOGE_USDT", 1.0 }, { "BTC_USDT", 0.0001 } });

    exchange::GateRestClient rest(ExchangeConfig{}, MarketType::Futures);
    QuantoResolver resolver(rest, m_cachePath.string());
    const auto result = resolver.resolveQuanto("DOGE_USDT", MarketType::Futures);
    ASSERT_TRUE(ok(result));
    EXPECT_DOUBLE_EQ(1.0, value(result));

    const auto btc = resolver.resolveQuanto("BTC_USDT", MarketType::Futures);
    ASSERT_TRUE(ok(btc));
    EXPECT_DOUBLE_EQ(0.0001, value(btc));
}

TEST_F(QuantoResolverTest, CacheHit_UnknownSymbol_GivesDidYouMean)
{
    writeFreshCache({ { "AAA_USDT", 1.0 }, { "BTC_USDT", 0.0001 },
                      { "DOGE_USDT", 1.0 }, { "ETH_USDT", 0.01 } });

    exchange::GateRestClient rest(ExchangeConfig{}, MarketType::Futures);
    QuantoResolver resolver(rest, m_cachePath.string());
    const auto result = resolver.resolveQuanto("NOT_A_COIN_USDT", MarketType::Futures);
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::BacktestContractUnknown, error(result).code);
    const std::string &message = error(result).message;
    EXPECT_NE(std::string::npos, message.find("NOT_A_COIN_USDT"));
    EXPECT_NE(std::string::npos, message.find("4 contracts available"));
    EXPECT_NE(std::string::npos, message.find("--quanto"));
}

TEST_F(QuantoResolverTest, FuturesContract_ReturnsFullMeta)
{
    writeFreshCache({ { "DOGE_USDT", 10.0 } });

    exchange::GateRestClient rest(ExchangeConfig{}, MarketType::Futures);
    QuantoResolver resolver(rest, m_cachePath.string());
    const auto meta = resolver.futuresContract("DOGE_USDT");
    ASSERT_TRUE(ok(meta));
    EXPECT_DOUBLE_EQ(10.0, value(meta).quanto_multiplier);
    EXPECT_DOUBLE_EQ(125.0, value(meta).leverage_max);
    EXPECT_EQ(1, value(meta).order_size_min);
}

} // namespace pulse::backtest::test
