// test_kline_store.cpp — KlineStore trailing accumulation + importFromDb,
// with a stubbed fetch source (no network) and a temp sqlite sink.

#include "backtest/KlineStore.hpp"
#include "backtest/SqliteKlineReader.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <gtest/gtest.h>

#include <filesystem>

namespace pulse::backtest::test
{

namespace
{

class KlineStoreTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        const auto name = ::testing::UnitTest::GetInstance()
                              ->current_test_info()
                              ->name();
        m_dbPath = std::filesystem::temp_directory_path()
            / ("pulse_ks_" + std::string{ name } + ".db");
        m_legacyPath = std::filesystem::temp_directory_path()
            / ("pulse_ks_legacy_" + std::string{ name } + ".db");
        std::filesystem::remove(m_dbPath);
        std::filesystem::remove(m_legacyPath);
    }

    void TearDown() override
    {
        std::filesystem::remove(m_dbPath);
        std::filesystem::remove(m_legacyPath);
    }

    std::filesystem::path m_dbPath;
    std::filesystem::path m_legacyPath;
};

market::Kline makeCandle(std::int64_t open_ms, double close = 100.0)
{
    market::Kline k;
    k.open_time = open_ms;
    k.close_time = open_ms + 60'000;
    k.open = close;
    k.high = close + 1.0;
    k.low = close - 1.0;
    k.close = close;
    k.volume = 10.0;
    k.closed = true;
    return k;
}

/// Stub fetch source: serves fixed candles within the requested window.
class StubApiSource final : public IKlineSource
{
  public:
    std::vector<market::Kline> candles_to_serve;
    int fetch_calls = 0;

    Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override
    {
        (void)symbol;
        (void)market_type;
        ++fetch_calls;
        std::vector<market::Kline> out;
        for (const auto &c : candles_to_serve)
        {
            if (from_ms <= c.open_time && c.open_time <= to_ms)
            {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string description() const override
    {
        return "stub-api";
    }
};

/// A fetch source that always fails (retry path).
class FailingApiSource final : public IKlineSource
{
  public:
    int fetch_calls = 0;

    Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override
    {
        (void)symbol;
        (void)market_type;
        (void)from_ms;
        (void)to_ms;
        ++fetch_calls;
        return PulseError{ ErrorCode::BacktestApiFetchFailed, "stub boom" };
    }

    std::string description() const override
    {
        return "failing-api";
    }
};

/// Create a legacy v1-schema source DB with one futures + one spot row per
/// symbol (the row mix that v1's PK could never have held in one table).
void createLegacySourceDb(const std::string &path)
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

    const auto add = [&](const std::string &symbol, const std::string &mt,
                         std::int64_t t, double close)
    {
        ins.bind(1, symbol);
        ins.bind(2, mt);
        ins.bind(3, t);
        ins.bind(4, t + 60'000);
        ins.bind(5, close);
        ins.bind(6, close + 1.0);
        ins.bind(7, close - 1.0);
        ins.bind(8, close);
        ins.bind(9, 10.0);
        ins.bind(10, 1);
        ins.exec();
        ins.reset();
    };

    add("ETH_USDT", "futures", 1'000'000, 100.0);
    add("ETH_USDT", "futures", 1'060'000, 101.0);
    add("SNDK_USDT", "futures", 2'000'000, 7.0);
    // Spot at a DIFFERENT minute: v1's (symbol, open_time) PK could not hold
    // a spot + futures row on the same minute anyway.
    add("ETH_USDT", "spot", 1'120'000, 100.5);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// storeWindow
// ---------------------------------------------------------------------------

TEST_F(KlineStoreTest, StoreWindow_WritesRowsAndReportsStats)
{
    StubApiSource api;
    api.candles_to_serve = { makeCandle(1'000'000), makeCandle(1'060'000) };
    SqliteKlineReader sink(m_dbPath.string());
    KlineStore store(api, sink);

    // now_ms well past the last candle → no forming-drop.
    const auto stats = store.storeWindow("DOGE_USDT", MarketType::Futures,
                                         1'000'000, 1'060'000, 1'200'000);
    ASSERT_TRUE(ok(stats));
    EXPECT_EQ(2u, value(stats).rows_fetched);
    EXPECT_EQ(2u, value(stats).rows_new);
    EXPECT_EQ(1'000'000LL, value(stats).first_open_ms);
    EXPECT_EQ(1'060'000LL, value(stats).last_open_ms);

    // The rows are really in the store, tagged futures.
    const auto all = sink.fetch("DOGE_USDT", MarketType::Futures, 0, 2'000'000);
    ASSERT_TRUE(ok(all));
    EXPECT_EQ(2u, value(all).size());
}

TEST_F(KlineStoreTest, StoreWindow_SecondRun_AddsNothing)
{
    StubApiSource api;
    api.candles_to_serve = { makeCandle(1'000'000), makeCandle(1'060'000) };
    SqliteKlineReader sink(m_dbPath.string());
    KlineStore store(api, sink);

    const auto first = store.storeWindow("DOGE_USDT", MarketType::Futures,
                                         1'000'000, 1'060'000, 1'200'000);
    ASSERT_TRUE(ok(first));
    EXPECT_EQ(2u, value(first).rows_new);

    const auto second = store.storeWindow("DOGE_USDT", MarketType::Futures,
                                          1'000'000, 1'060'000, 1'200'000);
    ASSERT_TRUE(ok(second));
    EXPECT_EQ(0u, value(second).rows_new);
}

TEST_F(KlineStoreTest, StoreWindow_DropsFormingCandle)
{
    // The futures layout has no reliable closed flag — the store must cut the
    // candle whose minute has not elapsed yet (open_time >= now - 60s).
    StubApiSource api;
    api.candles_to_serve = {
        makeCandle(1'000'000),
        makeCandle(1'060'000),  // finished: 60s before now
        makeCandle(1'120'000),  // forming: 0s before now → must be dropped
    };
    SqliteKlineReader sink(m_dbPath.string());
    KlineStore store(api, sink);

    const auto stats = store.storeWindow("DOGE_USDT", MarketType::Futures,
                                         1'000'000, 1'120'000, 1'180'000);
    ASSERT_TRUE(ok(stats));
    EXPECT_EQ(3u, value(stats).rows_fetched);
    EXPECT_EQ(2u, value(stats).rows_new);
    EXPECT_EQ(1'060'000LL, value(stats).last_open_ms);
}

TEST_F(KlineStoreTest, StoreWindow_FetchFailure_RetriesOnceThenErrors)
{
    FailingApiSource api;
    SqliteKlineReader sink(m_dbPath.string());
    KlineStore store(api, sink);

    const auto stats = store.storeWindow("DOGE_USDT", MarketType::Futures,
                                         1'000'000, 1'060'000, 1'200'000);
    ASSERT_FALSE(ok(stats));
    EXPECT_EQ(ErrorCode::BacktestApiFetchFailed, error(stats).code);
    EXPECT_EQ(2, api.fetch_calls); // original + retry with slack
}

// ---------------------------------------------------------------------------
// importFuturesFromDb
// ---------------------------------------------------------------------------

TEST_F(KlineStoreTest, ImportFromDb_CopiesOnlyFuturesAndSkipsExisting)
{
    createLegacySourceDb(m_legacyPath.string());
    SqliteKlineReader sink(m_dbPath.string());

    // Seed one ETH_USDT minute already present in the target.
    (void)sink.writeBack("ETH_USDT", MarketType::Futures, { makeCandle(1'000'000) });

    const auto imported = importFuturesFromDb(m_legacyPath.string(), {}, sink);
    ASSERT_TRUE(ok(imported));
    // ETH 2 rows (1 dup skipped) + SNDK 1 row. Spot rows are not imported.
    EXPECT_EQ(2u, value(imported));

    const auto eth = sink.fetch("ETH_USDT", MarketType::Futures, 0, 3'000'000);
    ASSERT_TRUE(ok(eth));
    EXPECT_EQ(2u, value(eth).size());

    const auto sndk = sink.fetch("SNDK_USDT", MarketType::Futures, 0, 3'000'000);
    ASSERT_TRUE(ok(sndk));
    EXPECT_EQ(1u, value(sndk).size());

    // Spot rows were deliberately not copied.
    const auto spot = sink.fetch("ETH_USDT", MarketType::Spot, 0, 3'000'000);
    ASSERT_TRUE(ok(spot));
    EXPECT_TRUE(value(spot).empty());

    // Re-import is idempotent.
    const auto again = importFuturesFromDb(m_legacyPath.string(), {}, sink);
    ASSERT_TRUE(ok(again));
    EXPECT_EQ(0u, value(again));
}

TEST_F(KlineStoreTest, ImportFromDb_SymbolFilter)
{
    createLegacySourceDb(m_legacyPath.string());
    SqliteKlineReader sink(m_dbPath.string());

    const auto imported =
        importFuturesFromDb(m_legacyPath.string(), { "SNDK_USDT" }, sink);
    ASSERT_TRUE(ok(imported));
    EXPECT_EQ(1u, value(imported));

    const auto sndk = sink.fetch("SNDK_USDT", MarketType::Futures, 0, 3'000'000);
    ASSERT_TRUE(ok(sndk));
    EXPECT_EQ(1u, value(sndk).size());
    const auto eth = sink.fetch("ETH_USDT", MarketType::Futures, 0, 3'000'000);
    ASSERT_TRUE(ok(eth));
    EXPECT_TRUE(value(eth).empty());
}

TEST_F(KlineStoreTest, ImportFromDb_MissingSource_IsAnError)
{
    SqliteKlineReader sink(m_dbPath.string());
    const auto imported =
        importFuturesFromDb((m_legacyPath.string() + "_nope"), {}, sink);
    ASSERT_FALSE(ok(imported));
}

} // namespace pulse::backtest::test
