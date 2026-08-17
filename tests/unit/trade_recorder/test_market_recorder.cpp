// test_market_recorder.cpp — MarketRecorder unit tests (M18 market data persistence)

#include "trade_recorder/MarketRecorder.hpp"

#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

using namespace pulse;
using namespace pulse::trade_recorder;
using namespace pulse::market;

namespace
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Unique temp db path with all side files removed (WAL/SHM).
std::string temp_db_path(const std::string &name)
{
    const std::string path = "/tmp/pulse_mr_" + name + ".db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
    return path;
}

Ticker make_ticker(const std::string &symbol, double last, std::int64_t ts_ms)
{
    Ticker t;
    t.symbol = symbol;
    t.last = last;
    t.bid = last - 0.5;
    t.ask = last + 0.5;
    t.volume_24h = 1234.5;
    t.change_pct = 0.42;
    t.mark_price = last;
    t.index_price = last;
    t.funding_rate = 0.0001;
    t.timestamp = ts_ms;
    return t;
}

Kline make_kline(const std::string &symbol, std::int64_t open_time, double close)
{
    Kline k;
    k.open_time = open_time;
    k.close_time = open_time + 60000;
    k.open = close - 1.0;
    k.high = close + 2.0;
    k.low = close - 2.0;
    k.close = close;
    k.volume = 100.0;
    k.closed = true;
    return k;
}

std::int64_t count_rows(const std::string &path, const std::string &table)
{
    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement stmt(db, "SELECT COUNT(*) FROM " + table);
    stmt.executeStep();
    return stmt.getColumn(0).getInt64();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Batching / draining
// ---------------------------------------------------------------------------

TEST(MarketRecorder, TickerFlushBatches)
{
    const std::string path = temp_db_path("flush");
    auto result = MarketRecorder::open(path, 8192, 64, std::chrono::seconds(10));
    ASSERT_TRUE(ok(result)) << error(result).message;
    auto &rec = value(result);

    for (int i = 0; i < 300; ++i)
    {
        rec->onTicker("XAUUSD", MarketType::Cfd,
                      make_ticker("XAUUSD", 4350.0 + i, 1000 + i));
    }
    rec->stop();

    // 256 flushed in batches of 64, the remaining 44 drained on stop.
    EXPECT_EQ(300, count_rows(path, "ticker_ticks"));
    EXPECT_EQ(0u, rec->droppedCount());
}

TEST(MarketRecorder, StopDrainsQueue)
{
    const std::string path = temp_db_path("drain");
    // batch 2048 > 1000 events → the writer never flushes mid-stream;
    // everything must land via the stop() drain.
    auto result = MarketRecorder::open(path, 8192, 2048, std::chrono::seconds(10));
    ASSERT_TRUE(ok(result)) << error(result).message;
    auto &rec = value(result);

    for (int i = 0; i < 1000; ++i)
    {
        rec->onKline("BTC_USDT", MarketType::Futures,
                     make_kline("BTC_USDT", 1000 + i, 50000.0));
    }
    EXPECT_EQ(1000u, rec->queueSize()); // All still queued.
    rec->stop();

    EXPECT_EQ(1000, count_rows(path, "kline_bars"));
}

// ---------------------------------------------------------------------------
// Dedupe / persistence
// ---------------------------------------------------------------------------

TEST(MarketRecorder, KlineDedupeByOpenTime)
{
    const std::string path = temp_db_path("kline_dedupe");
    auto result = MarketRecorder::open(path, 8192, 64, std::chrono::seconds(10));
    ASSERT_TRUE(ok(result)) << error(result).message;
    auto &rec = value(result);

    // Same (symbol, open_time) pushed twice — INSERT OR IGNORE keeps one row.
    // (WS forming-bar repeats / CFD backfill re-fetches arrive this way.)
    const auto kline = make_kline("XAUUSD", 1786740960000LL, 4376.0);
    rec->onKline("XAUUSD", MarketType::Cfd, kline);
    rec->onKline("XAUUSD", MarketType::Cfd, kline);
    rec->stop();

    EXPECT_EQ(1, count_rows(path, "kline_bars"));
}

TEST(MarketRecorder, PersistenceAcrossReopen)
{
    const std::string path = temp_db_path("persist");
    {
        auto result = MarketRecorder::open(path, 8192, 64, std::chrono::seconds(10));
        ASSERT_TRUE(ok(result)) << error(result).message;
        value(result)->onTicker("BTC_USDT", MarketType::Futures,
                                make_ticker("BTC_USDT", 50000.0, 111));
        value(result)->stop();
    }
    {
        auto result = MarketRecorder::open(path, 8192, 64, std::chrono::seconds(10));
        ASSERT_TRUE(ok(result)) << error(result).message;
        value(result)->onTicker("BTC_USDT", MarketType::Futures,
                                make_ticker("BTC_USDT", 50001.0, 222));
        value(result)->stop();
    }
    EXPECT_EQ(2, count_rows(path, "ticker_ticks"));
}

// ---------------------------------------------------------------------------
// Overflow / lifecycle
// ---------------------------------------------------------------------------

TEST(MarketRecorder, OverflowDropsOldest)
{
    const std::string path = temp_db_path("overflow");
    // capacity 8, batch 64 → the writer never flushes mid-stream.
    auto result = MarketRecorder::open(path, 8, 64, std::chrono::seconds(10));
    ASSERT_TRUE(ok(result)) << error(result).message;
    auto &rec = value(result);

    for (int i = 0; i < 10; ++i)
    {
        rec->onTicker("XAUUSD", MarketType::Cfd,
                      make_ticker("XAUUSD", 4350.0, 1000 + i));
    }
    rec->stop();

    EXPECT_EQ(8, count_rows(path, "ticker_ticks"));
    EXPECT_EQ(2u, rec->droppedCount()); // 10 enqueued - 8 capacity.
}

TEST(MarketRecorder, ZeroFlushNoCallbackAfterClose)
{
    const std::string path = temp_db_path("after_close");
    auto result = MarketRecorder::open(path, 8192, 64, std::chrono::seconds(10));
    ASSERT_TRUE(ok(result)) << error(result).message;
    auto &rec = value(result);

    rec->stop();

    // Enqueuing after stop must be a silent no-op, not a crash.
    EXPECT_NO_THROW(rec->onTicker("XAUUSD", MarketType::Cfd,
                                  make_ticker("XAUUSD", 4350.0, 1)));
    EXPECT_NO_THROW(rec->onKline("XAUUSD", MarketType::Cfd,
                                 make_kline("XAUUSD", 42, 4350.0)));
    EXPECT_EQ(0u, rec->queueSize());
    EXPECT_EQ(0, count_rows(path, "ticker_ticks"));
}
