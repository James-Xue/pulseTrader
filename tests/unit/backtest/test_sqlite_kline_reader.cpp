// test_sqlite_kline_reader.cpp — SqliteKlineReader against a temp-file DB.

#include "backtest/SqliteKlineReader.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

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

market::Kline makeCandle(std::int64_t open_ms, double close_price = 100.5)
{
    market::Kline k;
    k.open_time = open_ms;
    k.close_time = open_ms + 60'000;
    k.open = close_price;
    k.high = close_price + 1.0;
    k.low = close_price - 1.0;
    k.close = close_price;
    k.volume = 12.0;
    k.closed = true;
    return k;
}

/// Number of columns marked as PRIMARY KEY in the live kline_bars table.
int klinePkColumnCount(SQLite::Database &db)
{
    // table_info columns: cid, name, type, notnull, dflt_value, pk.
    SQLite::Statement pragma(db, "PRAGMA table_info(kline_bars)");
    int pk_columns = 0;
    while (pragma.executeStep())
    {
        if (pragma.getColumn(5).getInt() > 0)
        {
            ++pk_columns;
        }
    }
    return pk_columns;
}

/// Create a legacy v1-schema DB (PK (symbol, open_time)) with one futures row.
void createLegacyV1Db(const std::string &path, std::int64_t open_ms)
{
    SQLite::Database raw(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    raw.exec("CREATE TABLE kline_bars ("
             "symbol TEXT NOT NULL, market_type TEXT NOT NULL,"
             "open_time INTEGER NOT NULL, close_time INTEGER NOT NULL,"
             "open REAL NOT NULL, high REAL NOT NULL, low REAL NOT NULL,"
             "close REAL NOT NULL, volume REAL NOT NULL,"
             "closed INTEGER NOT NULL,"
             "PRIMARY KEY (symbol, open_time))");
    SQLite::Statement ins(raw,
        "INSERT INTO kline_bars (symbol, market_type, open_time, close_time, "
        "open, high, low, close, volume, closed) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, "ETH_USDT");
    ins.bind(2, "futures");
    ins.bind(3, open_ms);
    ins.bind(4, open_ms + 60'000);
    ins.bind(5, 100.0);
    ins.bind(6, 101.0);
    ins.bind(7, 99.0);
    ins.bind(8, 100.5);
    ins.bind(9, 12.0);
    ins.bind(10, 1);
    ins.exec();
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

// ---------------------------------------------------------------------------
// schema v2 (M33) — PK (symbol, market_type, open_time)
// ---------------------------------------------------------------------------

TEST_F(SqliteKlineReaderTest, V2_SameMinute_SpotAndFuturesCoexist)
{
    SqliteKlineReader reader(m_dbPath.string());
    const auto fut = reader.writeBack("ETH_USDT", MarketType::Futures,
        { makeCandle(1'000'000, 100.5) });
    ASSERT_TRUE(ok(fut));
    EXPECT_EQ(1u, value(fut));

    // Same (symbol, open_time) on the other market — v1 PK would have
    // IGNOREd this row; v2 keeps both.
    const auto spo = reader.writeBack("ETH_USDT", MarketType::Spot,
        { makeCandle(1'000'000, 200.5) });
    ASSERT_TRUE(ok(spo));
    EXPECT_EQ(1u, value(spo));

    const auto futures = reader.fetch("ETH_USDT", MarketType::Futures, 0, 2'000'000);
    ASSERT_TRUE(ok(futures));
    ASSERT_EQ(1u, value(futures).size());
    EXPECT_DOUBLE_EQ(100.5, value(futures)[0].close);

    const auto spot = reader.fetch("ETH_USDT", MarketType::Spot, 0, 2'000'000);
    ASSERT_TRUE(ok(spot));
    ASSERT_EQ(1u, value(spot).size());
    EXPECT_DOUBLE_EQ(200.5, value(spot)[0].close);

    // Introspect through a second connection — reader.open() is private.
    SQLite::Database raw(m_dbPath.string(), SQLite::OPEN_READWRITE);
    EXPECT_EQ(3, klinePkColumnCount(raw));
}

TEST_F(SqliteKlineReaderTest, V2_LegacyTable_MigratesOnOpenAndKeepsRows)
{
    createLegacyV1Db(m_dbPath.string(), 1'000'000);

    // Opening the reader upgrades the v1 table; the row that v1 could not
    // hold (spot at the same minute as the stored futures row) now inserts.
    SqliteKlineReader reader(m_dbPath.string());
    const auto spo = reader.writeBack("ETH_USDT", MarketType::Spot,
        { makeCandle(1'000'000, 200.5) });
    ASSERT_TRUE(ok(spo));
    EXPECT_EQ(1u, value(spo));

    // Both legacy and newly-inserted rows are visible per market.
    const auto futures = reader.fetch("ETH_USDT", MarketType::Futures, 0, 2'000'000);
    ASSERT_TRUE(ok(futures));
    ASSERT_EQ(1u, value(futures).size());
    EXPECT_DOUBLE_EQ(100.5, value(futures)[0].close);

    const auto spot = reader.fetch("ETH_USDT", MarketType::Spot, 0, 2'000'000);
    ASSERT_TRUE(ok(spot));
    ASSERT_EQ(1u, value(spot).size());
    EXPECT_DOUBLE_EQ(200.5, value(spot)[0].close);

    // The schema is really v2 now, not merely behaving like it.
    // Introspect through a second connection — reader.open() is private.
    SQLite::Database raw(m_dbPath.string(), SQLite::OPEN_READWRITE);
    EXPECT_EQ(3, klinePkColumnCount(raw));
}

TEST_F(SqliteKlineReaderTest, V2_MigrationIsIdempotent)
{
    createLegacyV1Db(m_dbPath.string(), 1'000'000);

    // Two reader lifetimes over the same file: the second open must not
    // re-migrate (PK count stays 3, legacy row survives, writes still work).
    {
        SqliteKlineReader first(m_dbPath.string());
        (void)first.writeBack("ETH_USDT", MarketType::Spot, { makeCandle(1'000'000) });
        SQLite::Database raw(m_dbPath.string(), SQLite::OPEN_READWRITE);
        EXPECT_EQ(3, klinePkColumnCount(raw));
    }
    {
        SqliteKlineReader second(m_dbPath.string());
        SQLite::Database raw(m_dbPath.string(), SQLite::OPEN_READWRITE);
        EXPECT_EQ(3, klinePkColumnCount(raw));

        const auto futures = second.fetch("ETH_USDT", MarketType::Futures, 0, 2'000'000);
        ASSERT_TRUE(ok(futures));
        EXPECT_EQ(1u, value(futures).size());
    }
}

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
