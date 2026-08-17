// test_trade_recorder.cpp — Core tests for TradeRecorder (Phase 2)

#include "trade_recorder/TradeRecorder.hpp"

#include "execution/ExecutionReport.hpp"

#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <chrono>
#include <filesystem>

using namespace pulse;
using namespace pulse::trade_recorder;
using namespace pulse::execution;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

ExecutionReport make_report(
    const std::string &order_id = "ORD001",
    double filled_qty = 0.001,
    double avg_price = 65000.0,
    double fees = 0.26,
    OrderStatus status = OrderStatus::Filled)
{
    ExecutionReport r;
    r.order_id = order_id;
    r.client_order_id = "momentum_scalper";
    r.symbol = "BTC_USDT";
    r.side = Side::Buy;
    r.type = OrderType::Market;
    r.requested_qty = 0.001;
    r.filled_qty = filled_qty;
    r.avg_fill_price = avg_price;
    r.submit_mid_price = 64990.0;
    r.slippage_bps = 1.54;
    r.fees = fees;
    r.latency = std::chrono::milliseconds(120);
    r.submit_time = std::chrono::system_clock::now();
    r.fill_time = r.submit_time + r.latency;
    r.final_status = status;
    return r;
}

TradeRecorder open_memory_db()
{
    auto result = TradeRecorder::open(":memory:");
    EXPECT_TRUE(ok(result));
    return std::move(value(result));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 1. Open / close
// ---------------------------------------------------------------------------

TEST(TradeRecorder, OpenCreatesDatabaseFile)
{
    const std::string path = "/tmp/test_pulse_trades.db";
    std::filesystem::remove(path);

    auto result = TradeRecorder::open(path);
    ASSERT_TRUE(ok(result));
    EXPECT_TRUE(std::filesystem::exists(path));

    value(result).close();
    std::filesystem::remove(path);
}

TEST(TradeRecorder, OpenCreatesTableAndIndexes)
{
    auto recorder = open_memory_db();
    EXPECT_EQ(0, recorder.tradeCount());
}

TEST(TradeRecorder, OpenExistingDatabase)
{
    const std::string path = "/tmp/test_pulse_reopen.db";
    std::filesystem::remove(path);

    // Open, insert, close.
    {
        auto result = TradeRecorder::open(path);
        ASSERT_TRUE(ok(result));
        auto &rec = value(result);
        EXPECT_TRUE(ok(rec.recordTrade(make_report(), 1.5, "test")));
    }

    // Reopen — data should persist.
    {
        auto result = TradeRecorder::open(path);
        ASSERT_TRUE(ok(result));
        EXPECT_EQ(1, value(result).tradeCount());
    }

    std::filesystem::remove(path);
}

TEST(TradeRecorder, OpenInvalidPath)
{
    auto result = TradeRecorder::open("/no/such/directory/trades.db");
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::TradeRecorderDbError, error(result).code);
}

// ---------------------------------------------------------------------------
// 2. Record trades
// ---------------------------------------------------------------------------

TEST(TradeRecorder, RecordTradeFilled)
{
    auto recorder = open_memory_db();
    auto report = make_report();
    report.final_status = OrderStatus::Filled;

    auto result = recorder.recordTrade(report, 2.5, "momentum_scalper");
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(1, recorder.tradeCount());
}

TEST(TradeRecorder, RecordTradeCancelled)
{
    auto recorder = open_memory_db();
    auto report = make_report("ORD002", 0.0, 0.0, 0.0, OrderStatus::Cancelled);

    auto result = recorder.recordTrade(report, 0.0, "");
    ASSERT_TRUE(ok(result));
    EXPECT_EQ(1, recorder.tradeCount());
}

TEST(TradeRecorder, RecordTradeDuplicateOrderId)
{
    auto recorder = open_memory_db();
    auto report = make_report("DUP001");

    EXPECT_TRUE(ok(recorder.recordTrade(report, 1.0, "strat")));

    auto dup_result = recorder.recordTrade(report, 1.0, "strat");
    ASSERT_FALSE(ok(dup_result));
    EXPECT_EQ(ErrorCode::TradeRecorderDuplicate, error(dup_result).code);
}

TEST(TradeRecorder, RecordTradeAllFieldsPreserved)
{
    auto recorder = open_memory_db();
    auto report = make_report("FULL001", 0.005, 42000.0, 1.05);
    report.client_order_id = "my_client_id";
    report.side = Side::Sell;
    report.type = OrderType::Limit;
    report.submit_mid_price = 41990.0;
    report.slippage_bps = 2.38;
    report.latency = std::chrono::milliseconds(250);

    // M18: pass market metadata explicitly (futures, 10x, quanto 0.0001).
    EXPECT_TRUE(ok(recorder.recordTrade(report, 5.75, "orderbook_scalper",
                                        MarketType::Futures, 10.0, 0.0001)));

    auto query = recorder.getTrades();
    ASSERT_TRUE(ok(query));
    auto &trades = value(query);
    ASSERT_EQ(1u, trades.size());

    const auto &t = trades[0];
    EXPECT_EQ("FULL001", t.order_id);
    EXPECT_EQ("my_client_id", t.client_order_id);
    EXPECT_EQ("BTC_USDT", t.symbol);
    EXPECT_EQ("sell", t.side);
    EXPECT_EQ("limit", t.order_type);
    EXPECT_DOUBLE_EQ(0.001, t.requested_qty);
    EXPECT_DOUBLE_EQ(0.005, t.filled_qty);
    EXPECT_DOUBLE_EQ(42000.0, t.avg_fill_price);
    EXPECT_DOUBLE_EQ(41990.0, t.submit_mid_price);
    EXPECT_NEAR(2.38, t.slippage_bps, 0.01);
    EXPECT_DOUBLE_EQ(1.05, t.fees);
    EXPECT_DOUBLE_EQ(5.75, t.pnl);
    EXPECT_EQ(250, t.latency_ms);
    EXPECT_EQ("filled", t.final_status);
    EXPECT_EQ("orderbook_scalper", t.strategy_name);
    // M18 columns.
    EXPECT_EQ("futures", t.market_type);
    EXPECT_DOUBLE_EQ(10.0, t.leverage);
    EXPECT_DOUBLE_EQ(0.0001, t.quanto_multiplier);
}

TEST(TradeRecorder, RecordTradeZeroPnl)
{
    auto recorder = open_memory_db();
    EXPECT_TRUE(ok(recorder.recordTrade(make_report(), 0.0, "")));

    auto query = recorder.getTrades();
    ASSERT_TRUE(ok(query));
    EXPECT_DOUBLE_EQ(0.0, value(query)[0].pnl);
}

TEST(TradeRecorder, RecordTradeNegativePnl)
{
    auto recorder = open_memory_db();
    EXPECT_TRUE(ok(recorder.recordTrade(make_report(), -5.2, "strat")));

    auto query = recorder.getTrades();
    ASSERT_TRUE(ok(query));
    EXPECT_DOUBLE_EQ(-5.2, value(query)[0].pnl);
}

TEST(TradeRecorder, RecordTradeMultiple)
{
    auto recorder = open_memory_db();

    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(ok(
            recorder.recordTrade(
                make_report("MULTI" + std::to_string(i)), 1.0, "s")));
    }

    EXPECT_EQ(5, recorder.tradeCount());
}

TEST(TradeRecorder, CloseAndReopen)
{
    const std::string path = "/tmp/test_pulse_close_reopen.db";
    std::filesystem::remove(path);

    {
        auto result = TradeRecorder::open(path);
        ASSERT_TRUE(ok(result));
        auto &rec = value(result);
        EXPECT_TRUE(ok(rec.recordTrade(make_report("P001"), 1.0, "s1")));
        EXPECT_TRUE(ok(rec.recordTrade(make_report("P002"), 2.0, "s2")));
        rec.checkpoint();
        rec.close();
    }

    {
        auto result = TradeRecorder::open(path);
        ASSERT_TRUE(ok(result));
        EXPECT_EQ(2, value(result).tradeCount());
    }

    std::filesystem::remove(path);
}

TEST(TradeRecorder, CheckpointDoesNotThrow)
{
    auto recorder = open_memory_db();
    EXPECT_TRUE(ok(recorder.recordTrade(make_report(), 0.0, "")));
    recorder.checkpoint();
    EXPECT_EQ(1, recorder.tradeCount());
}

TEST(TradeRecorder, RecordTradeEmptyStrategyName)
{
    auto recorder = open_memory_db();
    EXPECT_TRUE(ok(recorder.recordTrade(make_report(), 0.0, "")));

    auto query = recorder.getTrades();
    ASSERT_TRUE(ok(query));
    EXPECT_EQ("", value(query)[0].strategy_name);
}

TEST(TradeRecorder, RecordTradePartialFill)
{
    auto recorder = open_memory_db();
    auto report = make_report("PARTIAL001", 0.0005, 65000.0, 0.13);
    report.requested_qty = 0.001;  // requested 0.001 but only filled 0.0005

    EXPECT_TRUE(ok(recorder.recordTrade(report, 0.5, "strat")));

    auto query = recorder.getTrades();
    ASSERT_TRUE(ok(query));
    const auto &t = value(query)[0];
    EXPECT_DOUBLE_EQ(0.001, t.requested_qty);
    EXPECT_DOUBLE_EQ(0.0005, t.filled_qty);
}

// ---------------------------------------------------------------------------
// 3. M18 columns — market metadata + schema migration
// ---------------------------------------------------------------------------

TEST(TradeRecorder, RecordTradeSpotDefaults)
{
    // 3-arg call → spot semantics (market_type=spot, leverage=1.0, quanto=1.0).
    auto recorder = open_memory_db();
    EXPECT_TRUE(ok(recorder.recordTrade(make_report("SPOT001"), 1.0, "s")));

    auto query = recorder.getTrades();
    ASSERT_TRUE(ok(query));
    const auto &t = value(query)[0];
    EXPECT_EQ("spot", t.market_type);
    EXPECT_DOUBLE_EQ(1.0, t.leverage);
    EXPECT_DOUBLE_EQ(1.0, t.quanto_multiplier);
}

TEST(TradeRecorder, FreshDatabaseSetsUserVersion)
{
    const std::string path = "/tmp/test_pulse_user_version.db";
    std::filesystem::remove(path);

    {
        auto result = TradeRecorder::open(path);
        ASSERT_TRUE(ok(result));
        value(result).close();
    }

    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement stmt(db, "PRAGMA user_version");
    ASSERT_TRUE(stmt.executeStep());
    EXPECT_EQ(1, stmt.getColumn(0).getInt64());
}

TEST(TradeRecorder, MigrationUpgradesOldSchema)
{
    // Build a database with the pre-M18 17-column schema + one old row,
    // then open it with the current recorder: columns are added in place,
    // the old row is preserved with spot defaults, new inserts still work.
    const std::string path = "/tmp/test_pulse_old_schema.db";
    std::filesystem::remove(path);

    {
        SQLite::Database db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec(R"(
CREATE TABLE trades (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    order_id        TEXT    NOT NULL UNIQUE,
    client_order_id TEXT    DEFAULT '',
    timestamp       INTEGER NOT NULL,
    symbol          TEXT    NOT NULL,
    side            TEXT    NOT NULL,
    order_type      TEXT    NOT NULL,
    requested_qty   REAL    NOT NULL,
    filled_qty      REAL    NOT NULL,
    avg_fill_price  REAL    NOT NULL,
    submit_mid_price REAL   DEFAULT 0,
    slippage_bps    REAL    DEFAULT 0,
    fees            REAL    NOT NULL,
    pnl             REAL    DEFAULT 0,
    latency_ms      INTEGER NOT NULL,
    final_status    TEXT    NOT NULL,
    strategy_name   TEXT    DEFAULT ''
);
)");
        db.exec(R"(
INSERT INTO trades (order_id, client_order_id, timestamp, symbol, side,
                    order_type, requested_qty, filled_qty, avg_fill_price,
                    submit_mid_price, slippage_bps, fees, pnl, latency_ms,
                    final_status, strategy_name)
VALUES ('OLD001', '', 123, 'BTC_USDT', 'buy', 'market', 0.001, 0.001,
        65000, 64990, 1.5, 0.26, 2.5, 120, 'filled', 'old_strat');
)");
    }

    auto result = TradeRecorder::open(path);
    ASSERT_TRUE(ok(result)) << error(result).message;
    auto &rec = value(result);

    // Old row preserved with spot defaults for the new columns.
    auto query = rec.getTrades();
    ASSERT_TRUE(ok(query));
    ASSERT_EQ(1u, value(query).size());
    const auto &t = value(query)[0];
    EXPECT_EQ("OLD001", t.order_id);
    EXPECT_EQ("spot", t.market_type);
    EXPECT_DOUBLE_EQ(0.0, t.leverage);
    EXPECT_DOUBLE_EQ(1.0, t.quanto_multiplier);

    // New inserts work after migration.
    EXPECT_TRUE(ok(rec.recordTrade(make_report("NEW001"), 1.0, "s")));
    EXPECT_EQ(2, rec.tradeCount());

    // Schema now has 20 columns (17 + market_type/leverage/quanto).
    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement col_stmt(db, "PRAGMA table_info(trades)");
    int ncols = 0;
    while (col_stmt.executeStep())
    {
        ++ncols;
    }
    EXPECT_EQ(20, ncols);
}

TEST(TradeRecorder, MigrationIdempotentReopen)
{
    const std::string path = "/tmp/test_pulse_migration_reopen.db";
    std::filesystem::remove(path);

    // First open creates + migrates the schema.
    {
        auto result = TradeRecorder::open(path);
        ASSERT_TRUE(ok(result));
        EXPECT_TRUE(ok(value(result).recordTrade(make_report("V1A"), 1.0, "s")));
    }

    // Second open must not re-run ALTER on already-migrated columns.
    {
        auto result = TradeRecorder::open(path);
        ASSERT_TRUE(ok(result)) << error(result).message;
        auto &rec = value(result);
        EXPECT_EQ(1, rec.tradeCount());
        EXPECT_TRUE(ok(rec.recordTrade(make_report("V1B"), 2.0, "s")));
        EXPECT_EQ(2, rec.tradeCount());
    }
}
