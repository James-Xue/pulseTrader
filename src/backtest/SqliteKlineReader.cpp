// sqlite_kline_reader.cpp — kline_bars read/write for backtesting (M29)

#include "backtest/SqliteKlineReader.hpp"

#include "logging/Logger.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

namespace pulse::backtest
{

namespace
{

// ---------------------------------------------------------------------------
// kline_bars schema v2 (M33)
//
// v1 PK was (symbol, open_time) — market_type was not part of the key, so
// INSERT OR IGNORE silently dropped one market's row whenever spot and
// futures shared the same minute (on the VPS, M31 daily-sync futures rows
// were being discarded behind continuously-recorded spot rows). v2 widens
// the PK to (symbol, market_type, open_time).
// Mirrored in src/trade_recorder/MarketRecorder.cpp — keep both in sync.
// ---------------------------------------------------------------------------

constexpr const char *kKlineTableV2 = R"(CREATE TABLE IF NOT EXISTS kline_bars (
    symbol      TEXT NOT NULL,
    market_type TEXT NOT NULL,
    open_time   INTEGER NOT NULL,
    close_time  INTEGER NOT NULL,
    open        REAL NOT NULL,
    high        REAL NOT NULL,
    low         REAL NOT NULL,
    close       REAL NOT NULL,
    volume      REAL NOT NULL,
    closed      INTEGER NOT NULL,
    PRIMARY KEY (symbol, market_type, open_time)
))";

constexpr const char *kKlineIndexV2 = R"(CREATE INDEX IF NOT EXISTS
    idx_kline_sym_type_time ON kline_bars(symbol, market_type, open_time))";

/// Idempotent schema upgrade: detect the v1 PK via PRAGMA table_info
/// (CREATE TABLE IF NOT EXISTS alone cannot upgrade an existing v1 table)
/// and rebuild with the v2 PK. Legacy rows are copied verbatim — pairs that
/// v1 could not hold together were already lost and history is not relabelled.
/// Fresh DBs (created v2 above) take the no-op path.
void ensureKlineSchemaV2(SQLite::Database &db)
{
    bool table_exists = false;
    int pk_columns = 0;
    {
        // table_info columns: cid, name, type, notnull, dflt_value, pk.
        SQLite::Statement pragma(db, "PRAGMA table_info(kline_bars)");
        while (pragma.executeStep())
        {
            table_exists = true;
            if (pragma.getColumn(5).getInt() > 0)
            {
                ++pk_columns;
            }
        }
    }

    if (table_exists && pk_columns == 2)
    {
        SQLite::Transaction tx(db);
        db.exec("ALTER TABLE kline_bars RENAME TO kline_bars_v1");
        db.exec(kKlineTableV2);
        db.exec("INSERT INTO kline_bars (symbol, market_type, open_time, "
                "close_time, open, high, low, close, volume, closed) "
                "SELECT symbol, market_type, open_time, close_time, open, "
                "high, low, close, volume, closed FROM kline_bars_v1");
        db.exec("DROP TABLE kline_bars_v1");
        tx.commit();
        PULSE_LOG_INFO("backtest", "kline_bars migrated to schema v2 "
            "(PK symbol,market_type,open_time)");
    }

    db.exec(kKlineIndexV2);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SqliteKlineReader::SqliteKlineReader(std::string db_path)
    : m_dbPath{ std::move(db_path) }
{
}

SqliteKlineReader::~SqliteKlineReader() = default;

bool SqliteKlineReader::isOpen() const
{
    return m_db != nullptr;
}

// ---------------------------------------------------------------------------
// open — lazy connection
// ---------------------------------------------------------------------------

Result<SQLite::Database *> SqliteKlineReader::open()
{
    if (m_db)
    {
        return m_db.get();
    }
    if (m_openFailed)
    {
        return PulseError{ ErrorCode::BacktestSqliteUnavailable,
            "kline_bars database not available: " + m_dbPath };
    }

    try
    {
        m_db = std::make_unique<SQLite::Database>(
            m_dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        m_db->exec("PRAGMA journal_mode=WAL");
        m_db->exec("PRAGMA busy_timeout=5000");
        // Same schema DDL as MarketRecorder so write-back works standalone,
        // plus the one-time v1 → v2 PK migration and the composite index.
        m_db->exec(kKlineTableV2);
        ensureKlineSchemaV2(*m_db);
        return m_db.get();
    }
    catch (const SQLite::Exception &e)
    {
        m_openFailed = true;
        return PulseError{ ErrorCode::BacktestSqliteUnavailable,
            "Failed to open kline_bars database " + m_dbPath + ": " + e.what() };
    }
}

// ---------------------------------------------------------------------------
// fetch
// ---------------------------------------------------------------------------

Result<std::vector<market::Kline>> SqliteKlineReader::fetch(
    const std::string &symbol, MarketType market_type,
    std::int64_t from_ms, std::int64_t to_ms)
{
    auto db = open();
    if (!ok(db))
    {
        // No local data is a legitimate outcome — degrade to empty, not error.
        return std::vector<market::Kline>{};
    }

    const std::string market_label = toString(market_type);
    const char *kSelectKlines = "SELECT open_time, close_time, open, high, low, "
                                "close, volume, closed FROM kline_bars "
                                "WHERE symbol = ? AND market_type = ? "
                                "AND open_time BETWEEN ? AND ? "
                                "ORDER BY open_time ASC";

    try
    {
        SQLite::Statement stmt(*value(db), kSelectKlines);
        stmt.bind(1, symbol);
        stmt.bind(2, market_label);
        stmt.bind(3, from_ms);
        stmt.bind(4, to_ms);

        std::vector<market::Kline> candles;
        while (stmt.executeStep())
        {
            market::Kline kline;
            kline.open_time = stmt.getColumn(0).getInt64();
            kline.close_time = stmt.getColumn(1).getInt64();
            kline.open = stmt.getColumn(2).getDouble();
            kline.high = stmt.getColumn(3).getDouble();
            kline.low = stmt.getColumn(4).getDouble();
            kline.close = stmt.getColumn(5).getDouble();
            kline.volume = stmt.getColumn(6).getDouble();
            kline.closed = (0 != stmt.getColumn(7).getInt());
            candles.push_back(kline);
        }
        return candles;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ ErrorCode::BacktestSqliteUnavailable,
            "kline_bars SELECT failed: " + std::string{ e.what() } };
    }
}

std::string SqliteKlineReader::description() const
{
    return "sqlite:" + m_dbPath;
}

// ---------------------------------------------------------------------------
// coverage
// ---------------------------------------------------------------------------

Result<std::optional<std::pair<std::int64_t, std::int64_t>>> SqliteKlineReader::coverage(
    const std::string &symbol, MarketType market_type)
{
    auto db = open();
    if (!ok(db))
    {
        return std::nullopt;
    }

    const std::string market_label = toString(market_type);
    const char *kRangeQuery = "SELECT MIN(open_time), MAX(open_time) FROM kline_bars "
                              "WHERE symbol = ? AND market_type = ?";

    try
    {
        SQLite::Statement stmt(*value(db), kRangeQuery);
        stmt.bind(1, symbol);
        stmt.bind(2, market_label);

        if (stmt.executeStep())
        {
            if (stmt.getColumn(0).isNull())
            {
                return std::nullopt;
            }
            return std::optional<std::pair<std::int64_t, std::int64_t>>{
                std::make_pair(stmt.getColumn(0).getInt64(),
                               stmt.getColumn(1).getInt64())
            };
        }
        return std::nullopt;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ ErrorCode::BacktestSqliteUnavailable,
            "kline_bars coverage query failed: " + std::string{ e.what() } };
    }
}

// ---------------------------------------------------------------------------
// writeBack
// ---------------------------------------------------------------------------

Result<std::size_t> SqliteKlineReader::writeBack(
    const std::string &symbol, MarketType market_type,
    const std::vector<market::Kline> &candles)
{
    if (candles.empty())
    {
        return std::size_t{ 0 };
    }

    auto db = open();
    if (!ok(db))
    {
        return PulseError{ ErrorCode::BacktestSqliteUnavailable,
            "Cannot write back: " + error(db).message };
    }

    const std::string market_label = toString(market_type);
    const char *kInsertKline = "INSERT OR IGNORE INTO kline_bars "
                               "(symbol, market_type, open_time, close_time, open, "
                               "high, low, close, volume, closed) "
                               "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

    try
    {
        SQLite::Transaction transaction(*value(db));
        SQLite::Statement stmt(*value(db), kInsertKline);

        std::size_t inserted = 0;
        for (const auto &k : candles)
        {
            stmt.bind(1, symbol);
            stmt.bind(2, market_label);
            stmt.bind(3, k.open_time);
            stmt.bind(4, k.close_time);
            stmt.bind(5, k.open);
            stmt.bind(6, k.high);
            stmt.bind(7, k.low);
            stmt.bind(8, k.close);
            stmt.bind(9, k.volume);
            stmt.bind(10, k.closed ? 1 : 0);
            if (stmt.exec() > 0)
            {
                ++inserted;
            }
            stmt.reset();
        }
        transaction.commit();
        return inserted;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ ErrorCode::BacktestSqliteUnavailable,
            "kline_bars writeBack failed: " + std::string{ e.what() } };
    }
}

} // namespace pulse::backtest
