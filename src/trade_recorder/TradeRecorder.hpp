#pragma once
// trade_recorder.hpp — SQLite-backed trade persistence (Phase 2)
//
// Records every completed order (Filled / Cancelled) into a SQLite database
// for post-trade analysis. Thread-safe via mutex; WAL mode for concurrent reads.

#include "core/PulseError.hpp"
#include "core/types.hpp"
#include "execution/ExecutionReport.hpp"
#include "trade_recorder/TradeRecord.hpp"

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
//       recorder.recordTrade(report, pnl, "momentum_scalper");
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
    ///
    /// market_type / leverage / quanto_multiplier identify the trading market
    /// (defaults = spot semantics). leverage <= 0 is normalized to 1.0.
    [[nodiscard]] Result<bool> recordTrade(
        const execution::ExecutionReport &report,
        double pnl,
        const std::string &strategy_name,
        MarketType market_type = MarketType::Spot,
        double leverage = 1.0,
        double quanto_multiplier = 1.0);

    /// Query trades filtered by symbol and/or time range.
    /// Empty symbol + zero timestamps returns all trades.
    [[nodiscard]] Result<std::vector<TradeRecord>> getTrades(
        const std::string &symbol = "",
        std::int64_t from_ns = 0,
        std::int64_t to_ns = 0) const;

    /// Query trades by strategy name.
    [[nodiscard]] Result<std::vector<TradeRecord>> getTradesByStrategy(
        const std::string &strategy_name) const;

    /// Aggregate summary (count, pnl, win_rate, fees) over a time range.
    [[nodiscard]] Result<TradeSummary> getSummary(
        std::int64_t from_ns = 0,
        std::int64_t to_ns = 0) const;

    /// Daily PnL for a given date (UTC midnight boundaries).
    [[nodiscard]] Result<double> getDailyPnl(
        std::int64_t date_ns) const;

    /// Per-strategy aggregates over a time range — the AI tuning feedback
    /// signal (GROUP BY strategy_name, market_type). Empty window = all.
    [[nodiscard]] Result<std::vector<StrategyTradeSummary>> getStrategySummary(
        std::int64_t from_ns = 0,
        std::int64_t to_ns = 0) const;

    /// Total number of recorded trades.
    [[nodiscard]] std::int64_t tradeCount() const;

    /// Flush WAL frames to main DB file (call before shutdown).
    void checkpoint();

    /// Close the database connection (optional — destructor does this).
    void close();

  private:
    explicit TradeRecorder(std::unique_ptr<SQLite::Database> db);

    [[nodiscard]] Result<bool> createSchema();

    /// Versioned schema migration (v0 → v1: market_type/leverage/quanto
    /// columns). Idempotent — safe on fresh and already-migrated databases.
    [[nodiscard]] Result<bool> migrateSchema();

    mutable std::mutex m_mutex;
    std::unique_ptr<SQLite::Database> m_db;
};

} // namespace pulse::trade_recorder
