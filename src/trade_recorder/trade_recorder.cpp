// trade_recorder.cpp — TradeRecorder implementation (Phase 2)

#include "trade_recorder/trade_recorder.hpp"

#include "logging/logger.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Exception.h>
#include <SQLiteCpp/Statement.h>

namespace pulse::trade_recorder
{

namespace
{

// ---------------------------------------------------------------------------
// Enum-to-string helpers
// ---------------------------------------------------------------------------

std::string sideToString(Side s)
{
    return Side::Buy == s ? "buy" : "sell";
}

std::string orderTypeToString(OrderType t)
{
    switch (t)
    {
        case OrderType::Market:
            return "market";
        case OrderType::Limit:
            return "limit";
        case OrderType::PostOnly:
            return "post_only";
    }
    return "unknown";
}

std::string statusToString(OrderStatus s)
{
    switch (s)
    {
        case OrderStatus::Filled:
            return "filled";
        case OrderStatus::Cancelled:
            return "cancelled";
        default:
            return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Schema DDL
// ---------------------------------------------------------------------------

constexpr const char *kCreateTable = R"(
CREATE TABLE IF NOT EXISTS trades (
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
)";

constexpr const char *kCreateIndexes = R"(
CREATE INDEX IF NOT EXISTS idx_trades_symbol_ts  ON trades(symbol, timestamp);
CREATE INDEX IF NOT EXISTS idx_trades_strategy   ON trades(strategy_name);
CREATE INDEX IF NOT EXISTS idx_trades_timestamp  ON trades(timestamp);
)";

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TradeRecorder::TradeRecorder(std::unique_ptr<SQLite::Database> db)
    : m_db{ std::move(db) }
{
}

TradeRecorder::~TradeRecorder()
{
    close();
}

TradeRecorder::TradeRecorder(TradeRecorder &&other) noexcept
    : m_db{ std::move(other.m_db) }
{
}

TradeRecorder &TradeRecorder::operator=(TradeRecorder &&other) noexcept
{
    if (this != &other)
    {
        close();
        m_db = std::move(other.m_db);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

Result<TradeRecorder> TradeRecorder::open(const std::string &db_path)
{
    try
    {
        auto db = std::make_unique<SQLite::Database>(
            db_path,
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        db->exec("PRAGMA journal_mode=WAL");
        db->exec("PRAGMA synchronous=NORMAL");
        db->exec("PRAGMA foreign_keys=ON");

        TradeRecorder recorder(std::move(db));
        auto schema_result = recorder.createSchema();

        if (!ok(schema_result))
        {
            return error(schema_result);
        }

        return recorder;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ErrorCode::TradeRecorderDbError, e.what()};
    }
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

Result<bool> TradeRecorder::createSchema()
{
    try
    {
        m_db->exec(kCreateTable);
        m_db->exec(kCreateIndexes);
        return true;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ErrorCode::TradeRecorderSchemaError, e.what()};
    }
}

// ---------------------------------------------------------------------------
// Record trade
// ---------------------------------------------------------------------------

Result<bool> TradeRecorder::recordTrade(
    const execution::ExecutionReport &report,
    double pnl,
    const std::string &strategy_name)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_db)
    {
        return PulseError{ErrorCode::TradeRecorderNotOpen,
                          "database is not open"};
    }

    try
    {
        auto fill_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           report.fill_time.time_since_epoch())
                           .count();

        SQLite::Statement stmt(*m_db,
            "INSERT INTO trades "
            "(order_id, client_order_id, timestamp, symbol, side, "
            "order_type, requested_qty, filled_qty, avg_fill_price, "
            "submit_mid_price, slippage_bps, fees, pnl, latency_ms, "
            "final_status, strategy_name) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

        stmt.bind(1, report.order_id);
        stmt.bind(2, report.client_order_id);
        stmt.bind(3, fill_ns);
        stmt.bind(4, report.symbol);
        stmt.bind(5, sideToString(report.side));
        stmt.bind(6, orderTypeToString(report.type));
        stmt.bind(7, report.requested_qty);
        stmt.bind(8, report.filled_qty);
        stmt.bind(9, report.avg_fill_price);
        stmt.bind(10, report.submit_mid_price);
        stmt.bind(11, report.slippage_bps);
        stmt.bind(12, report.fees);
        stmt.bind(13, pnl);
        stmt.bind(14, static_cast<std::int64_t>(report.latency.count()));
        stmt.bind(15, statusToString(report.final_status));
        stmt.bind(16, strategy_name);

        stmt.exec();

        PULSE_LOG_INFO("trade_recorder",
                       "Recorded trade: {} {} {} {:.6f} @ {:.2f} "
                       "pnl={:.4f} strategy={}",
                       report.order_id,
                       report.symbol,
                       sideToString(report.side),
                       report.filled_qty,
                       report.avg_fill_price,
                       pnl,
                       strategy_name);

        return true;
    }
    catch (const SQLite::Exception &e)
    {
        std::string msg = e.what();

        // Detect UNIQUE constraint violation (SQLite error code 19).
        if (std::string::npos != msg.find("UNIQUE"))
        {
            return PulseError{ErrorCode::TradeRecorderDuplicate,
                              "duplicate order_id: " + report.order_id};
        }

        return PulseError{ErrorCode::TradeRecorderInsertFailed, msg};
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Result<std::vector<TradeRecord>> TradeRecorder::getTrades(
    const std::string &symbol,
    std::int64_t from_ns,
    std::int64_t to_ns) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_db)
    {
        return PulseError{ErrorCode::TradeRecorderNotOpen,
                          "database is not open"};
    }

    try
    {
        std::string sql = "SELECT * FROM trades WHERE 1=1";
        std::vector<std::string> params;

        if (!symbol.empty())
        {
            sql += " AND symbol = ?";
            params.push_back(symbol);
        }

        if (0 < from_ns)
        {
            sql += " AND timestamp >= ?";
        }

        if (0 < to_ns)
        {
            sql += " AND timestamp <= ?";
        }

        sql += " ORDER BY timestamp DESC";

        SQLite::Statement stmt(*m_db, sql);

        int idx = 1;

        for (const auto &p : params)
        {
            stmt.bind(idx++, p);
        }

        if (0 < from_ns)
        {
            stmt.bind(idx++, from_ns);
        }

        if (0 < to_ns)
        {
            stmt.bind(idx++, to_ns);
        }

        std::vector<TradeRecord> results;

        while (stmt.executeStep())
        {
            TradeRecord rec;
            rec.id = stmt.getColumn(0).getInt64();
            rec.order_id = stmt.getColumn(1).getString();
            rec.client_order_id = stmt.getColumn(2).getString();
            rec.timestamp_ns = stmt.getColumn(3).getInt64();
            rec.symbol = stmt.getColumn(4).getString();
            rec.side = stmt.getColumn(5).getString();
            rec.order_type = stmt.getColumn(6).getString();
            rec.requested_qty = stmt.getColumn(7).getDouble();
            rec.filled_qty = stmt.getColumn(8).getDouble();
            rec.avg_fill_price = stmt.getColumn(9).getDouble();
            rec.submit_mid_price = stmt.getColumn(10).getDouble();
            rec.slippage_bps = stmt.getColumn(11).getDouble();
            rec.fees = stmt.getColumn(12).getDouble();
            rec.pnl = stmt.getColumn(13).getDouble();
            rec.latency_ms = stmt.getColumn(14).getInt64();
            rec.final_status = stmt.getColumn(15).getString();
            rec.strategy_name = stmt.getColumn(16).getString();
            results.push_back(std::move(rec));
        }

        return results;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ErrorCode::TradeRecorderQueryFailed, e.what()};
    }
}

Result<std::vector<TradeRecord>> TradeRecorder::getTradesByStrategy(
    const std::string &strategy_name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_db)
    {
        return PulseError{ErrorCode::TradeRecorderNotOpen,
                          "database is not open"};
    }

    try
    {
        SQLite::Statement stmt(*m_db,
            "SELECT * FROM trades WHERE strategy_name = ? "
            "ORDER BY timestamp DESC");
        stmt.bind(1, strategy_name);

        std::vector<TradeRecord> results;

        while (stmt.executeStep())
        {
            TradeRecord rec;
            rec.id = stmt.getColumn(0).getInt64();
            rec.order_id = stmt.getColumn(1).getString();
            rec.client_order_id = stmt.getColumn(2).getString();
            rec.timestamp_ns = stmt.getColumn(3).getInt64();
            rec.symbol = stmt.getColumn(4).getString();
            rec.side = stmt.getColumn(5).getString();
            rec.order_type = stmt.getColumn(6).getString();
            rec.requested_qty = stmt.getColumn(7).getDouble();
            rec.filled_qty = stmt.getColumn(8).getDouble();
            rec.avg_fill_price = stmt.getColumn(9).getDouble();
            rec.submit_mid_price = stmt.getColumn(10).getDouble();
            rec.slippage_bps = stmt.getColumn(11).getDouble();
            rec.fees = stmt.getColumn(12).getDouble();
            rec.pnl = stmt.getColumn(13).getDouble();
            rec.latency_ms = stmt.getColumn(14).getInt64();
            rec.final_status = stmt.getColumn(15).getString();
            rec.strategy_name = stmt.getColumn(16).getString();
            results.push_back(std::move(rec));
        }

        return results;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ErrorCode::TradeRecorderQueryFailed, e.what()};
    }
}

Result<TradeSummary> TradeRecorder::getSummary(
    std::int64_t from_ns,
    std::int64_t to_ns) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_db)
    {
        return PulseError{ErrorCode::TradeRecorderNotOpen,
                          "database is not open"};
    }

    try
    {
        std::string sql =
            "SELECT COUNT(*), "
            "COALESCE(SUM(pnl), 0), "
            "COALESCE(SUM(fees), 0), "
            "COALESCE(AVG(CASE WHEN pnl > 0 THEN 1.0 ELSE 0.0 END), 0), "
            "COALESCE(AVG(slippage_bps), 0), "
            "COALESCE(AVG(latency_ms), 0), "
            "COALESCE(SUM(filled_qty * avg_fill_price), 0) "
            "FROM trades WHERE 1=1";

        if (0 < from_ns)
        {
            sql += " AND timestamp >= ?";
        }

        if (0 < to_ns)
        {
            sql += " AND timestamp <= ?";
        }

        SQLite::Statement stmt(*m_db, sql);

        int idx = 1;

        if (0 < from_ns)
        {
            stmt.bind(idx++, from_ns);
        }

        if (0 < to_ns)
        {
            stmt.bind(idx++, to_ns);
        }

        TradeSummary summary{};

        if (stmt.executeStep())
        {
            summary.tradeCount = stmt.getColumn(0).getInt64();
            summary.total_pnl = stmt.getColumn(1).getDouble();
            summary.total_fees = stmt.getColumn(2).getDouble();
            summary.win_rate = stmt.getColumn(3).getDouble();
            summary.avg_slippage_bps = stmt.getColumn(4).getDouble();
            summary.avg_latency_ms = stmt.getColumn(5).getDouble();
            summary.total_volume = stmt.getColumn(6).getDouble();
        }

        return summary;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ErrorCode::TradeRecorderQueryFailed, e.what()};
    }
}

Result<double> TradeRecorder::getDailyPnl(std::int64_t date_ns) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_db)
    {
        return PulseError{ErrorCode::TradeRecorderNotOpen,
                          "database is not open"};
    }

    try
    {
        // Calculate start and end of day (UTC midnight boundaries).
        constexpr std::int64_t kNsPerDay = 86400LL * 1'000'000'000LL;
        std::int64_t day_start = (date_ns / kNsPerDay) * kNsPerDay;
        std::int64_t day_end = day_start + kNsPerDay;

        SQLite::Statement stmt(*m_db,
            "SELECT COALESCE(SUM(pnl), 0) FROM trades "
            "WHERE timestamp >= ? AND timestamp < ?");
        stmt.bind(1, day_start);
        stmt.bind(2, day_end);

        double pnl = 0.0;

        if (stmt.executeStep())
        {
            pnl = stmt.getColumn(0).getDouble();
        }

        return pnl;
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ErrorCode::TradeRecorderQueryFailed, e.what()};
    }
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::int64_t TradeRecorder::tradeCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_db)
    {
        return 0;
    }

    try
    {
        SQLite::Statement stmt(*m_db, "SELECT COUNT(*) FROM trades");

        if (stmt.executeStep())
        {
            return stmt.getColumn(0).getInt64();
        }

        return 0;
    }
    catch (const SQLite::Exception &)
    {
        return 0;
    }
}

void TradeRecorder::checkpoint()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_db)
    {
        return;
    }

    try
    {
        m_db->exec("PRAGMA wal_checkpoint(TRUNCATE)");
    }
    catch (const SQLite::Exception &)
    {
        // Best-effort — don't throw during shutdown.
    }
}

void TradeRecorder::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_db.reset();
}

} // namespace pulse::trade_recorder
