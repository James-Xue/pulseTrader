#pragma once
// sqlite_kline_reader.hpp — kline_bars read/write for backtesting (M29)
//
// Reads the engine's own historical candle capture (kline_bars table written
// by MarketRecorder) and writes back API-fetched candles (INSERT OR IGNORE —
// same dedup semantics as the recorder). SQLite is optional in this project:
// compile this translation unit only when PULSE_ENABLE_SQLITE is ON (the
// header itself is safe to include unconditionally).

#include "backtest/KlineSource.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Forward-declare SQLite to avoid leaking it into the public header.
namespace SQLite
{
class Database;
}

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// SqliteKlineReader — IKlineSource backed by the local kline_bars table
// ---------------------------------------------------------------------------
class SqliteKlineReader final : public IKlineSource
{
  public:
    /// Open the database read-write (write-back needs INSERT OR IGNORE).
    /// The connection is opened lazily on first use so that a missing file
    /// degrades to "no local data" instead of aborting the backtest.
    explicit SqliteKlineReader(std::string db_path);

    ~SqliteKlineReader() override;

    SqliteKlineReader(const SqliteKlineReader &) = delete;
    SqliteKlineReader &operator=(const SqliteKlineReader &) = delete;

    /// Closed candles in [from_ms, to_ms] for one symbol + market, sorted by
    /// open_time. Empty vector (no error) when the range has no rows.
    [[nodiscard]] Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override;

    [[nodiscard]] std::string description() const override;

    /// Overall [min, max] open_time coverage for a symbol + market, or
    /// std::nullopt when the table has no rows for it. Used to auto-resolve
    /// an unspecified backtest window.
    [[nodiscard]] Result<std::optional<std::pair<std::int64_t, std::int64_t>>>
    coverage(const std::string &symbol, MarketType market_type);

    /// Persist candles with INSERT OR IGNORE (deduped by (symbol, open_time)).
    /// Returns the number of rows actually inserted.
    [[nodiscard]] Result<std::size_t> writeBack(
        const std::string &symbol, MarketType market_type,
        const std::vector<market::Kline> &candles) override;

    /// True once the database has been opened successfully at least once
    /// (used to decide whether write-back is possible).
    [[nodiscard]] bool isOpen() const;

  private:
    [[nodiscard]] Result<SQLite::Database *> open();

    std::string m_dbPath;
    std::unique_ptr<SQLite::Database> m_db;
    bool m_openFailed = false; ///< Cache a failed open — don't retry every call.
};

} // namespace pulse::backtest
