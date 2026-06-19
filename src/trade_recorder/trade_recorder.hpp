#pragma once
// trade_recorder.hpp — SQLite-backed trade persistence (Phase 2)
//
// Records every completed order (Filled / Cancelled) into a SQLite database
// for post-trade analysis. Thread-safe via mutex; WAL mode for concurrent reads.

#include "core/error.hpp"
#include "core/types.hpp"
#include "execution/execution_report.hpp"
#include "trade_recorder/trade_record.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward-declare SQLite to avoid leaking it into the public header.
namespace SQLite
{
class Database;
}

namespace pulse::trade_recorder
{

// ---------------------------------------------------------------------------
// TradeRecorder — RAII wrapper around a SQLite database connection
//
// Usage:
//   auto result = TradeRecorder::open("data/trades.db");
//   if (ok(result)) {
//       auto &recorder = value(result);
//       recorder.record_trade(report, pnl, "momentum_scalper");
//   }
// ---------------------------------------------------------------------------
class TradeRecorder
{
  public:
    /// Open or create the SQLite database at db_path.
    /// Creates the trades table + indexes if they don't exist.
    /// Enables WAL mode + PRAGMA synchronous=NORMAL.
    [[nodiscard]] static Result<TradeRecorder> open(
        const std::string &db_path);

    ~TradeRecorder();

    TradeRecorder(TradeRecorder &&) noexcept;
    TradeRecorder &operator=(TradeRecorder &&) noexcept;
    TradeRecorder(const TradeRecorder &) = delete;
    TradeRecorder &operator=(const TradeRecorder &) = delete;

    /// Record a completed trade. Thread-safe (mutex-guarded).
    /// Returns true on success, PulseError on failure.
    [[nodiscard]] Result<bool> record_trade(
        const execution::ExecutionReport &report,
        double pnl,
        const std::string &strategy_name);

    /// Query trades filtered by symbol and/or time range.
    /// Empty symbol + zero timestamps returns all trades.
    [[nodiscard]] Result<std::vector<TradeRecord>> get_trades(
        const std::string &symbol = "",
        std::int64_t from_ns = 0,
        std::int64_t to_ns = 0) const;

    /// Query trades by strategy name.
    [[nodiscard]] Result<std::vector<TradeRecord>> get_trades_by_strategy(
        const std::string &strategy_name) const;

    /// Aggregate summary (count, pnl, win_rate, fees) over a time range.
    [[nodiscard]] Result<TradeSummary> get_summary(
        std::int64_t from_ns = 0,
        std::int64_t to_ns = 0) const;

    /// Daily PnL for a given date (UTC midnight boundaries).
    [[nodiscard]] Result<double> get_daily_pnl(
        std::int64_t date_ns) const;

    /// Total number of recorded trades.
    [[nodiscard]] std::int64_t trade_count() const;

    /// Flush WAL frames to main DB file (call before shutdown).
    void checkpoint();

    /// Close the database connection (optional — destructor does this).
    void close();

  private:
    explicit TradeRecorder(std::unique_ptr<SQLite::Database> db);

    [[nodiscard]] Result<bool> create_schema();

    mutable std::mutex mutex_;
    std::unique_ptr<SQLite::Database> db_;
};

} // namespace pulse::trade_recorder
