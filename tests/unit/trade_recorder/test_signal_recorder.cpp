// test_signal_recorder.cpp — SignalRecorder unit tests (M32 signal journal)

#include "trade_recorder/SignalRecorder.hpp"

#include <gtest/gtest.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

using namespace pulse;
using namespace pulse::trade_recorder;

namespace
{

/// Unique temp db path with all side files removed (WAL/SHM).
std::string temp_db_path(const std::string &name)
{
    const std::string path = "/tmp/pulse_sr_" + name + ".db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
    return path;
}

SignalRecord make_signal(const std::string &strategy_id,
                         const std::string &type,
                         std::int64_t ts_ns,
                         double confidence = 0.8,
                         double price = 100.0)
{
    return SignalRecord{
        .ts_ns = ts_ns,
        .strategy_id = strategy_id,
        .symbol = "BTC_USDT",
        .market_type = "futures",
        .type = type,
        .confidence = confidence,
        .price = price,
        .reason = "test signal from " + strategy_id,
        .indicators = R"({"ema_fast": 99.5, "ema_slow": 100.5})",
    };
}

// getText() returns a const char* whose lifetime is tied to the statement;
// compare through std::string to compare content, not pointers.
std::string col_text(const SQLite::Statement &stmt, int idx)
{
    return std::string(stmt.getColumn(idx).getText());
}

/// Poll until the table row count reaches `expected` (flush is async).
std::int64_t wait_for_rows(const std::string &path, std::int64_t expected,
                           int timeout_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        SQLite::Database db(path, SQLite::OPEN_READONLY);
        SQLite::Statement stmt(db, "SELECT COUNT(*) FROM signals");
        stmt.executeStep();
        const auto count = stmt.getColumn(0).getInt64();
        if (count >= expected)
        {
            return count;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement stmt(db, "SELECT COUNT(*) FROM signals");
    stmt.executeStep();
    return stmt.getColumn(0).getInt64();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

TEST(SignalRecorder, PersistsAllFields)
{
    const std::string path = temp_db_path("fields");

    auto result = SignalRecorder::open(path);
    ASSERT_TRUE(pulse::ok(result));
    auto recorder = std::move(pulse::value(result));

    const std::int64_t ts_ns = 1'752'000'000'123'456'789LL;
    recorder->record(make_signal("eth_scalper_BTC_USDT", "buy", ts_ns,
                                 0.65, 79600.0));
    recorder->stop();

    EXPECT_EQ(wait_for_rows(path, 1), 1);

    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement stmt(
        db, "SELECT ts_ns, strategy_id, symbol, market_type, type, "
            "confidence, price, reason, indicators FROM signals");
    ASSERT_TRUE(stmt.executeStep());

    EXPECT_EQ(stmt.getColumn(0).getInt64(), ts_ns);
    EXPECT_EQ(col_text(stmt, 1), "eth_scalper_BTC_USDT");
    EXPECT_EQ(col_text(stmt, 2), "BTC_USDT");
    EXPECT_EQ(col_text(stmt, 3), "futures");
    EXPECT_EQ(col_text(stmt, 4), "buy");
    EXPECT_DOUBLE_EQ(stmt.getColumn(5).getDouble(), 0.65);
    EXPECT_DOUBLE_EQ(stmt.getColumn(6).getDouble(), 79600.0);
    EXPECT_EQ(col_text(stmt, 7), "test signal from eth_scalper_BTC_USDT");
    EXPECT_EQ(col_text(stmt, 8), R"({"ema_fast": 99.5, "ema_slow": 100.5})");
}

TEST(SignalRecorder, StopDrainsPendingWithoutWait)
{
    const std::string path = temp_db_path("drain");

    auto result = SignalRecorder::open(
        path, std::chrono::milliseconds(60'000)); // Very long flush interval
    ASSERT_TRUE(pulse::ok(result));
    auto recorder = std::move(pulse::value(result));

    for (int i = 0; i < 10; ++i)
    {
        recorder->record(make_signal("momentum_scalper_BTC_USDT",
                                     (i % 2) ? "sell" : "buy", 1000LL + i,
                                     0.5 + 0.03 * i, 100.0 + i));
    }
    recorder->stop(); // Must drain everything queued.

    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement stmt(db, "SELECT COUNT(*) FROM signals");
    stmt.executeStep();
    EXPECT_EQ(stmt.getColumn(0).getInt64(), 10);
}

TEST(SignalRecorder, AppendAcrossRestartsAndPreservesOrder)
{
    const std::string path = temp_db_path("restart");

    {
        auto result = SignalRecorder::open(path);
        ASSERT_TRUE(pulse::ok(result));
        auto recorder = std::move(pulse::value(result));
        recorder->record(make_signal("a_strategy", "buy", 100LL));
        recorder->stop();
    }
    {
        auto result = SignalRecorder::open(path);
        ASSERT_TRUE(pulse::ok(result));
        auto recorder = std::move(pulse::value(result));
        recorder->record(make_signal("b_strategy", "flat", 200LL));
        recorder->stop();
    }

    EXPECT_EQ(wait_for_rows(path, 2), 2);

    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement stmt(db, "SELECT strategy_id FROM signals ORDER BY id");
    ASSERT_TRUE(stmt.executeStep());
    EXPECT_EQ(col_text(stmt, 0), "a_strategy");
    ASSERT_TRUE(stmt.executeStep());
    EXPECT_EQ(col_text(stmt, 0), "b_strategy");
    EXPECT_FALSE(stmt.executeStep()); // No third row.
}

// ---------------------------------------------------------------------------
// Lifecycle / diagnostics
// ---------------------------------------------------------------------------

TEST(SignalRecorder, StopIsIdempotent)
{
    const std::string path = temp_db_path("idempotent");

    auto result = SignalRecorder::open(path);
    ASSERT_TRUE(pulse::ok(result));
    auto recorder = std::move(pulse::value(result));

    recorder->record(make_signal("s", "buy", 1LL));
    recorder->stop();
    recorder->stop(); // Second stop must be a no-op.

    // record() after stop() is a silent no-op, never a crash.
    recorder->record(make_signal("s", "sell", 2LL));
}

TEST(SignalRecorder, RecordAfterStopDoesNotBlockOrGrow)
{
    const std::string path = temp_db_path("after_stop");

    auto result = SignalRecorder::open(path);
    ASSERT_TRUE(pulse::ok(result));
    auto recorder = std::move(pulse::value(result));
    recorder->stop();

    recorder->record(make_signal("s", "buy", 1LL));
    recorder->record(make_signal("s", "flat", 2LL));

    SQLite::Database db(path, SQLite::OPEN_READONLY);
    SQLite::Statement stmt(db, "SELECT COUNT(*) FROM signals");
    stmt.executeStep();
    EXPECT_EQ(stmt.getColumn(0).getInt64(), 0);
    EXPECT_EQ(recorder->queueSize(), 0u);
}
