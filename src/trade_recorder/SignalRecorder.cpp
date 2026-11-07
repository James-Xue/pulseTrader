// signal_recorder.cpp — SignalRecorder implementation (M32 signal journal)

#include "trade_recorder/SignalRecorder.hpp"

#include "logging/Logger.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Exception.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

namespace pulse::trade_recorder
{

namespace
{

constexpr const char *kCreateTables = R"(
CREATE TABLE IF NOT EXISTS signals (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ns       INTEGER NOT NULL,
    strategy_id TEXT NOT NULL,
    symbol      TEXT NOT NULL,
    market_type TEXT NOT NULL,
    type        TEXT NOT NULL,
    confidence  REAL NOT NULL,
    price       REAL NOT NULL,
    reason      TEXT NOT NULL,
    indicators  TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_signals_strategy_ts ON signals(strategy_id, ts_ns);
CREATE INDEX IF NOT EXISTS idx_signals_symbol_ts  ON signals(symbol, ts_ns);
)";

constexpr const char *kInsertSignal = R"(
INSERT INTO signals
    (ts_ns, strategy_id, symbol, market_type, type, confidence, price,
     reason, indicators)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
)";

} // anonymous namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

Result<std::unique_ptr<SignalRecorder>> SignalRecorder::open(
    const std::string &db_path, std::chrono::milliseconds flush_interval)
{
    try
    {
        auto db = std::make_unique<SQLite::Database>(
            db_path,
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        db->exec("PRAGMA journal_mode=WAL");
        db->exec("PRAGMA synchronous=NORMAL");
        // Third connection to the same file as TradeRecorder/MarketRecorder —
        // WAL supports concurrent writers; busy_timeout arbitrates contention.
        db->exec("PRAGMA busy_timeout=5000");
        db->exec(kCreateTables);

        // Direct new-expression (member context): std::make_unique cannot
        // access the private constructor.
        return std::unique_ptr<SignalRecorder>(
            new SignalRecorder(std::move(db), flush_interval));
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ ErrorCode::TradeRecorderDbError, e.what() };
    }
}

SignalRecorder::SignalRecorder(std::unique_ptr<SQLite::Database> db,
                               std::chrono::milliseconds flush_interval)
    : m_db{ std::move(db) }
    , m_flushInterval{ flush_interval }
{
    m_writer = std::jthread([this](std::stop_token st)
                            { writerLoop(std::move(st)); });
}

SignalRecorder::~SignalRecorder()
{
    stop();
}

// ---------------------------------------------------------------------------
// Producer side (strategy threads — enqueue only, never blocks)
// ---------------------------------------------------------------------------

void SignalRecorder::record(const SignalRecord &rec)
{
    std::lock_guard lock(m_mutex);
    if (m_closing)
    {
        return; // No-op after stop() — callers must never block.
    }
    if (m_queue.size() >= kMaxPending)
    {
        // Writer stalled — drop the oldest rather than block strategies.
        m_queue.pop_front();
        ++m_droppedCount;
    }
    m_queue.push_back(rec);
    if (m_queue.size() >= kBatchSize)
    {
        m_cv.notify_one();
    }
}

// ---------------------------------------------------------------------------
// Writer thread
// ---------------------------------------------------------------------------

void SignalRecorder::writerLoop(std::stop_token stoken)
{
    bool finished = false;
    while (!finished)
    {
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, m_flushInterval, [this, &stoken]
                          { return m_closing || stoken.stop_requested()
                                || m_queue.size() >= kBatchSize; });
            finished = m_closing || stoken.stop_requested();
        }

        if (finished || !m_queue.empty())
        {
            flushQueue();
        }
    }

    // Final checkpoint so the main DB file is consistent before close.
    try
    {
        m_db->exec("PRAGMA wal_checkpoint(TRUNCATE)");
    }
    catch (const SQLite::Exception &e)
    {
        PULSE_LOG_WARN("signal_recorder", "WAL checkpoint failed: {}", e.what());
    }
    PULSE_LOG_INFO("signal_recorder", "Writer stopped — final flush complete");
}

void SignalRecorder::flushQueue()
{
    try
    {
        SQLite::Transaction transaction(*m_db,
                                        SQLite::TransactionBehavior::IMMEDIATE);
        SQLite::Statement ins(*m_db, kInsertSignal);

        for (;;)
        {
            SignalRecord rec;
            {
                std::lock_guard lock(m_mutex);
                if (m_queue.empty())
                {
                    break;
                }
                rec = std::move(m_queue.front());
                m_queue.pop_front();
            }

            ins.reset();
            ins.bind(1, rec.ts_ns);
            ins.bind(2, rec.strategy_id);
            ins.bind(3, rec.symbol);
            ins.bind(4, rec.market_type);
            ins.bind(5, rec.type);
            ins.bind(6, rec.confidence);
            ins.bind(7, rec.price);
            ins.bind(8, rec.reason);
            ins.bind(9, rec.indicators);
            ins.exec();
        }

        transaction.commit();
    }
    catch (const SQLite::Exception &e)
    {
        PULSE_LOG_ERROR("signal_recorder", "Flush failed: {}", e.what());
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SignalRecorder::stop()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_closing)
        {
            return; // Idempotent.
        }
        m_closing = true;
    }
    m_cv.notify_all();
    if (m_writer.joinable())
    {
        m_writer.join(); // Writer drains everything, checkpoints, then exits.
    }
    m_db.reset(); // Close the connection.
}

std::size_t SignalRecorder::droppedCount() const
{
    std::lock_guard lock(m_mutex);
    return m_droppedCount;
}

std::size_t SignalRecorder::queueSize() const
{
    std::lock_guard lock(m_mutex);
    return m_queue.size();
}

} // namespace pulse::trade_recorder
