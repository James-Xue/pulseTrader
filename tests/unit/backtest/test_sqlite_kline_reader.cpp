// test_sqlite_kline_reader.cpp — SqliteKlineReader against a temp-file DB.

#include "backtest/SqliteKlineReader.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace pulse::backtest::test
{

namespace
{

class SqliteKlineReaderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Unique DB file per test to avoid cross-test contamination.
        m_dbPath = std::filesystem::temp_directory_path()
            / ("pulse_bt_" + std::string(::testing::UnitTest::GetInstance()
                                             ->current_test_info()
                                             ->name())
                   + ".db");
        std::filesystem::remove(m_dbPath);
    }

    void TearDown() override
    {
        std::filesystem::remove(m_dbPath);
    }

    std::filesystem::path m_dbPath;
};

market::Kline makeCandle(std::int64_t open_ms)
{
    market::Kline k;
    k.open_time = open_ms;
    k.close_time = open_ms + 60'000;
    k.open = 100.0;
    k.high = 101.0;
    k.low = 99.0;
    k.close = 100.5;
    k.volume = 12.0;
    k.closed = true;
    return k;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// fetch — range filtering, symbol/market filtering, ordering
// ---------------------------------------------------------------------------

TEST_F(SqliteKlineReaderTest, Fetch_FiltersRangeAndOrdersAscending)
{
    SqliteKlineReader reader(m_dbPath.string());
    (void)reader.writeBack("ETH_USDT", MarketType::Futures,
        { makeCandle(1'000'000), makeCandle(1'060'000), makeCandle(1'120'000) });

    // Out-of-order input gets sorted by the writer's caller; here we verify
    // the reader returns ascending regardless.
    const auto result = reader.fetch("ETH_USDT", MarketType::Futures,
        1'000'000, 1'060'000);
    ASSERT_TRUE(ok(result));
    const auto &candles = value(result);
    ASSERT_EQ(2u, candles.size());
    EXPECT_EQ(1'000'000LL, candles[0].open_time);
    EXPECT_EQ(1'060'000LL, candles[1].open_time);
}

TEST_F(SqliteKlineReaderTest, Fetch_SymbolAndMarketFiltered)
{
    SqliteKlineReader reader(m_dbPath.string());
    (void)reader.writeBack("ETH_USDT", MarketType::Futures, { makeCandle(1'000'000) });
    (void)reader.writeBack("BTC_USDT", MarketType::Futures, { makeCandle(1'000'000) });
    (void)reader.writeBack("ETH_USDT", MarketType::Spot, { makeCandle(1'000'000) });

    // Same candle written under three (symbol, market) keys — the reader must
    // return exactly the one matching the requested pair.
    const auto result = reader.fetch("ETH_USDT", MarketType::Futures, 0, 1'000'000);
    ASSERT_TRUE(ok(result));
    ASSERT_EQ(1u, value(result).size());
}

TEST_F(SqliteKlineReaderTest, Fetch_EmptyRange_EmptyVectorNoError)
{
    SqliteKlineReader reader(m_dbPath.string());
    (void)reader.writeBack("ETH_USDT", MarketType::Futures, { makeCandle(1'000'000) });

    const auto result = reader.fetch("ETH_USDT", MarketType::Futures, 5'000'000, 6'000'000);
    ASSERT_TRUE(ok(result));
    EXPECT_TRUE(value(result).empty());
}

TEST_F(SqliteKlineReaderTest, Fetch_MissingFile_EmptyNoError)
{
    SqliteKlineReader reader((m_dbPath.string() + "_nonexistent").c_str());
    const auto result = reader.fetch("ETH_USDT", MarketType::Futures, 0, 1'000'000);
    ASSERT_TRUE(ok(result));
    EXPECT_TRUE(value(result).empty());
}

// ---------------------------------------------------------------------------
// writeBack — INSERT OR IGNORE dedup
// ---------------------------------------------------------------------------

TEST_F(SqliteKlineReaderTest, WriteBack_DeduplicatesByIdentity)
{
    SqliteKlineReader reader(m_dbPath.string());
    const auto first = reader.writeBack("ETH_USDT", MarketType::Futures,
        { makeCandle(1'000'000), makeCandle(1'060'000) });
    ASSERT_TRUE(ok(first));
    EXPECT_EQ(2u, value(first));

    const auto second = reader.writeBack("ETH_USDT", MarketType::Futures,
        { makeCandle(1'000'000), makeCandle(1'120'000) });
    ASSERT_TRUE(ok(second));
    EXPECT_EQ(1u, value(second)); // only the new candle inserted.

    const auto all = reader.fetch("ETH_USDT", MarketType::Futures, 0, 2'000'000);
    ASSERT_TRUE(ok(all));
    EXPECT_EQ(3u, value(all).size());
}

// ---------------------------------------------------------------------------
// coverage
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// coverage
// ---------------------------------------------------------------------------

TEST_F(SqliteKlineReaderTest, Coverage_ReturnsMinMax)
{
    SqliteKlineReader reader(m_dbPath.string());
    (void)reader.writeBack("ETH_USDT", MarketType::Futures,
        { makeCandle(1'000'000), makeCandle(1'060'000), makeCandle(1'120'000) });

    const auto result = reader.coverage("ETH_USDT", MarketType::Futures);
    ASSERT_TRUE(ok(result));
    ASSERT_TRUE(value(result).has_value());
    EXPECT_EQ(1'000'000LL, value(result)->first);
    EXPECT_EQ(1'120'000LL, value(result)->second);
}

TEST_F(SqliteKlineReaderTest, Coverage_NoRows_Nullopt)
{
    SqliteKlineReader reader(m_dbPath.string());
    const auto result = reader.coverage("ETH_USDT", MarketType::Futures);
    ASSERT_TRUE(ok(result));
    EXPECT_FALSE(value(result).has_value());
}

} // namespace pulse::backtest::test
