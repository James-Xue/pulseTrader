#pragma once
// market_recorder.hpp — SQLite-backed market data recorder (M18)
//
// Consumes ticker/kline events via the MarketDataSink interface and persists
// them to SQLite. The hot path (onTicker/onKline) only enqueues into a POD
// ring buffer under a mutex — zero allocation, zero I/O, never blocks.
// A dedicated writer thread flushes batches (BEGIN IMMEDIATE transactions)
// on a size or time threshold.
//
// Tables:
//   ticker_ticks — one row per ticker event (id AUTOINCREMENT, indexed by
//                  (symbol, ts_ms)).
//   kline_bars   — one row per candle, PRIMARY KEY (symbol, open_time);
//                  writes use INSERT OR IGNORE so re-pushed candles (WS
//                  forming-bar repeats, CFD backfill re-fetches) dedupe.

#include "core/PulseError.hpp"
#include "core/types.hpp"
#include "market/KlineBuffer.hpp"
#include "market/MarketDataSink.hpp"
#include "market/TickerCache.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward-declare SQLite to avoid leaking it into the public header.
namespace SQLite
{
class Database;
}

namespace pulse::trade_recorder
{

// ---------------------------------------------------------------------------
// POD ring-buffer entry — fixed-size fields, trivially copyable, so the
// hot path never allocates (symbol copied into a char array).
// ---------------------------------------------------------------------------
enum class EntryKind : std::uint8_t
{
    Ticker,
    Kline,
};

struct TickerTick
{
    std::int64_t ts_ms;
    double last;
    double bid;
    double ask;
    double volume_24h;
    double change_pct;
    double mark_price;
    double index_price;
    double funding_rate;
};

struct KlineBar
{
    std::int64_t open_time;
    std::int64_t close_time;
    double open;
    double high;
    double low;
    double close;
    double volume;
    bool closed;
};

struct MarketDataEntry
{
    EntryKind kind;
    MarketType market_type;
    char symbol[32];
    TickerTick ticker;
    KlineBar kline;
};
static_assert(std::is_trivially_copyable_v<MarketDataEntry>,
              "MarketDataEntry must be trivially copyable (hot-path memcpy)");

// ---------------------------------------------------------------------------
// MarketRecorder — SQLite market data recorder (MarketDataSink impl)
//
// Usage:
//   auto result = MarketRecorder::open("data/trades.db");
//   if (ok(result)) {
//       auto recorder = std::move(value(result));  // unique_ptr
//       feed.setMarketDataSink(recorder.get());
//       ...
//       recorder->stop();   // drains + checkpoints (idempotent)
//   }
// ---------------------------------------------------------------------------
class MarketRecorder : public market::MarketDataSink
{
  public:
    /// Open (or create) the database and start the writer thread.
    ///
    /// The recorder uses a SECOND connection to the same file as
    /// TradeRecorder (WAL supports concurrent connections; busy_timeout=5000
    /// arbitrates contention). Returns an error on SQLite failure.
    [[nodiscard]] static Result<std::unique_ptr<MarketRecorder>> open(
        const std::string &db_path,
        std::size_t queue_capacity = 8192,
        std::size_t batch_size = 128,
        std::chrono::milliseconds flush_interval = std::chrono::milliseconds(1000));

    ~MarketRecorder() override;

    MarketRecorder(const MarketRecorder &) = delete;
    MarketRecorder &operator=(const MarketRecorder &) = delete;
    MarketRecorder(MarketRecorder &&) = delete;
    MarketRecorder &operator=(MarketRecorder &&) = delete;

    /// MarketDataSink: enqueue a ticker event (O(1), never blocks).
    /// No-op after stop().
    void onTicker(const Symbol &symbol, MarketType market_type,
                  const market::Ticker &ticker) override;

    /// MarketDataSink: enqueue a kline event (O(1), never blocks).
    /// No-op after stop().
    void onKline(const Symbol &symbol, MarketType market_type,
                 const market::Kline &kline) override;

    /// Stop the writer thread, drain + flush everything remaining,
    /// run wal_checkpoint(TRUNCATE) and close the connection.
    /// Idempotent — safe to call multiple times.
    void stop();

    /// Number of events dropped because the ring buffer was full
    /// (oldest entries dropped first). Diagnostic / test aid.
    [[nodiscard]] std::size_t droppedCount() const;

    /// Number of events currently queued (not yet flushed).
    /// Diagnostic / test aid.
    [[nodiscard]] std::size_t queueSize() const;

  private:
    MarketRecorder(std::unique_ptr<SQLite::Database> db,
                   std::size_t queue_capacity,
                   std::size_t batch_size,
                   std::chrono::milliseconds flush_interval);

    /// Writer thread: flush batches (batch_size rows or flush_interval),
    /// then on shutdown drain everything and checkpoint the WAL.
    void writerLoop(std::stop_token stoken);

    /// O(1) enqueue; drops the oldest entry when the ring is full.
    void enqueue(const MarketDataEntry &entry);

    /// Flush one chunk inside a BEGIN IMMEDIATE transaction.
    void writeChunk(const std::vector<MarketDataEntry> &chunk);

    std::unique_ptr<SQLite::Database> m_db;

    // --- Ring buffer (producer: feed I/O threads; consumer: writer thread) ---
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<MarketDataEntry> m_queue;
    std::size_t m_queueHead{ 0 };
    std::size_t m_queueSize{ 0 };
    std::size_t m_droppedCount{ 0 };
    bool m_closing{ false };

    std::size_t m_batchSize;
    std::chrono::milliseconds m_flushInterval;
    std::jthread m_writer;
};

} // namespace pulse::trade_recorder
