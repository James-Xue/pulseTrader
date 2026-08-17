// market_recorder.cpp — MarketRecorder implementation (M18 market data persistence)

#include "trade_recorder/MarketRecorder.hpp"

#include "logging/Logger.hpp"

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Exception.h>
#include <SQLiteCpp/Statement.h>
#include <SQLiteCpp/Transaction.h>

#include <algorithm>
#include <cstring>

namespace pulse::trade_recorder
{

namespace
{

// ---------------------------------------------------------------------------
// Schema DDL
// ---------------------------------------------------------------------------

constexpr const char *kCreateTables = R"(
CREATE TABLE IF NOT EXISTS ticker_ticks (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    symbol       TEXT NOT NULL,
    market_type  TEXT NOT NULL,
    ts_ms        INTEGER NOT NULL,
    last         REAL NOT NULL,
    bid          REAL NOT NULL,
    ask          REAL NOT NULL,
    volume_24h   REAL NOT NULL,
    change_pct   REAL NOT NULL,
    mark_price   REAL NOT NULL,
    index_price  REAL NOT NULL,
    funding_rate REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ticker_ticks_sym_ts ON ticker_ticks(symbol, ts_ms);

CREATE TABLE IF NOT EXISTS kline_bars (
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
    PRIMARY KEY (symbol, open_time)
);
)";

constexpr const char *kInsertTicker = R"(
INSERT INTO ticker_ticks
    (symbol, market_type, ts_ms, last, bid, ask, volume_24h,
     change_pct, mark_price, index_price, funding_rate)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)";

// INSERT OR IGNORE — PK (symbol, open_time) dedupes re-pushed candles
// (WS forming-bar repeats, CFD backfill re-fetches after restart).
constexpr const char *kInsertKline = R"(
INSERT OR IGNORE INTO kline_bars
    (symbol, market_type, open_time, close_time, open, high, low, close,
     volume, closed)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
)";

/// Copy a std::string into the fixed char array, NUL-terminated.
void copySymbol(char (&dst)[32], const std::string &src)
{
    const std::size_t n = std::min(src.size(), sizeof(dst) - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

Result<std::unique_ptr<MarketRecorder>> MarketRecorder::open(
    const std::string &db_path,
    std::size_t queue_capacity,
    std::size_t batch_size,
    std::chrono::milliseconds flush_interval)
{
    try
    {
        auto db = std::make_unique<SQLite::Database>(
            db_path,
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        db->exec("PRAGMA journal_mode=WAL");
        db->exec("PRAGMA synchronous=NORMAL");
        // Second connection to the same file as TradeRecorder — WAL supports
        // concurrent writers; busy_timeout arbitrates contention.
        db->exec("PRAGMA busy_timeout=5000");
        db->exec(kCreateTables);

        // Direct new-expression (member context): std::make_unique cannot
        // access the private constructor.
        return std::unique_ptr<MarketRecorder>(new MarketRecorder(
            std::move(db), queue_capacity, batch_size, flush_interval));
    }
    catch (const SQLite::Exception &e)
    {
        return PulseError{ErrorCode::TradeRecorderDbError, e.what()};
    }
}

MarketRecorder::MarketRecorder(std::unique_ptr<SQLite::Database> db,
                               std::size_t queue_capacity,
                               std::size_t batch_size,
                               std::chrono::milliseconds flush_interval)
    : m_db{ std::move(db) }
    , m_queue{ queue_capacity }
    , m_batchSize{ std::max<std::size_t>(batch_size, 1) }
    , m_flushInterval{ flush_interval }
{
    m_writer = std::jthread([this](std::stop_token st) { writerLoop(std::move(st)); });
}

MarketRecorder::~MarketRecorder()
{
    stop();
}

// ---------------------------------------------------------------------------
// MarketDataSink (hot path — enqueue only, never blocks)
// ---------------------------------------------------------------------------

void MarketRecorder::onTicker(const Symbol &symbol, MarketType market_type,
                              const market::Ticker &ticker)
{
    MarketDataEntry entry{};
    entry.kind = EntryKind::Ticker;
    entry.market_type = market_type;
    copySymbol(entry.symbol, symbol);
    entry.ticker = TickerTick{
        .ts_ms        = ticker.timestamp,
        .last         = ticker.last,
        .bid          = ticker.bid,
        .ask          = ticker.ask,
        .volume_24h   = ticker.volume_24h,
        .change_pct   = ticker.change_pct,
        .mark_price   = ticker.mark_price,
        .index_price  = ticker.index_price,
        .funding_rate = ticker.funding_rate,
    };
    enqueue(entry);
}

void MarketRecorder::onKline(const Symbol &symbol, MarketType market_type,
                             const market::Kline &kline)
{
    MarketDataEntry entry{};
    entry.kind = EntryKind::Kline;
    entry.market_type = market_type;
    copySymbol(entry.symbol, symbol);
    entry.kline = KlineBar{
        .open_time  = kline.open_time,
        .close_time = kline.close_time,
        .open       = kline.open,
        .high       = kline.high,
        .low        = kline.low,
        .close      = kline.close,
        .volume     = kline.volume,
        .closed     = kline.closed,
    };
    enqueue(entry);
}

void MarketRecorder::enqueue(const MarketDataEntry &entry)
{
    std::lock_guard lock(m_mutex);
    if (m_closing)
    {
        return; // No-op after stop() — callers must never block.
    }
    if (m_queueSize >= m_queue.size())
    {
        // Ring full — drop the oldest entry rather than blocking the feed.
        m_queueHead = (m_queueHead + 1) % m_queue.size();
        --m_queueSize;
        ++m_droppedCount;
    }
    m_queue[(m_queueHead + m_queueSize) % m_queue.size()] = entry;
    ++m_queueSize;
    if (m_queueSize >= m_batchSize)
    {
        m_cv.notify_one();
    }
}

// ---------------------------------------------------------------------------
// Writer thread
// ---------------------------------------------------------------------------

void MarketRecorder::writerLoop(std::stop_token stoken)
{
    std::vector<MarketDataEntry> chunk;
    chunk.reserve(m_batchSize);

    bool finished = false;
    while (!finished)
    {
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, m_flushInterval, [this, &stoken]
                          { return m_closing || stoken.stop_requested()
                                || m_queueSize >= m_batchSize; });

            const std::size_t avail = m_queueSize;
            if (m_closing || stoken.stop_requested())
            {
                // Shutdown — take EVERYTHING remaining, then exit.
                finished = true;
            }
            // Steady state: at most one batch per iteration; shutdown: all.
            const std::size_t count = finished ? avail : std::min(avail, m_batchSize);
            for (std::size_t i = 0; i < count; ++i)
            {
                chunk.push_back(m_queue[(m_queueHead + i) % m_queue.size()]);
            }
            m_queueHead = (m_queueHead + count) % m_queue.size();
            m_queueSize -= count;
        }

        if (!chunk.empty())
        {
            writeChunk(chunk);
            chunk.clear();
        }
    }

    // Final checkpoint so the main DB file is consistent before close.
    try
    {
        m_db->exec("PRAGMA wal_checkpoint(TRUNCATE)");
    }
    catch (const SQLite::Exception &e)
    {
        PULSE_LOG_WARN("market_recorder", "WAL checkpoint failed: {}", e.what());
    }
    PULSE_LOG_INFO("market_recorder", "Writer stopped — final flush complete");
}

void MarketRecorder::writeChunk(const std::vector<MarketDataEntry> &chunk)
{
    try
    {
        SQLite::Transaction transaction(*m_db, SQLite::TransactionBehavior::IMMEDIATE);
        SQLite::Statement ins_tick(*m_db, kInsertTicker);
        SQLite::Statement ins_bar(*m_db, kInsertKline);

        for (const auto &entry : chunk)
        {
            if (EntryKind::Ticker == entry.kind)
            {
                ins_tick.reset();
                ins_tick.bind(1, entry.symbol);
                ins_tick.bind(2, toString(entry.market_type));
                ins_tick.bind(3, entry.ticker.ts_ms);
                ins_tick.bind(4, entry.ticker.last);
                ins_tick.bind(5, entry.ticker.bid);
                ins_tick.bind(6, entry.ticker.ask);
                ins_tick.bind(7, entry.ticker.volume_24h);
                ins_tick.bind(8, entry.ticker.change_pct);
                ins_tick.bind(9, entry.ticker.mark_price);
                ins_tick.bind(10, entry.ticker.index_price);
                ins_tick.bind(11, entry.ticker.funding_rate);
                ins_tick.exec();
            }
            else
            {
                ins_bar.reset();
                ins_bar.bind(1, entry.symbol);
                ins_bar.bind(2, toString(entry.market_type));
                ins_bar.bind(3, entry.kline.open_time);
                ins_bar.bind(4, entry.kline.close_time);
                ins_bar.bind(5, entry.kline.open);
                ins_bar.bind(6, entry.kline.high);
                ins_bar.bind(7, entry.kline.low);
                ins_bar.bind(8, entry.kline.close);
                ins_bar.bind(9, entry.kline.volume);
                ins_bar.bind(10, entry.kline.closed ? 1 : 0);
                ins_bar.exec();
            }
        }

        transaction.commit();
    }
    catch (const SQLite::Exception &e)
    {
        PULSE_LOG_ERROR("market_recorder", "Batch flush failed ({} rows): {}", chunk.size(),
                        e.what());
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MarketRecorder::stop()
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

std::size_t MarketRecorder::droppedCount() const
{
    std::lock_guard lock(m_mutex);
    return m_droppedCount;
}

std::size_t MarketRecorder::queueSize() const
{
    std::lock_guard lock(m_mutex);
    return m_queueSize;
}

} // namespace pulse::trade_recorder
