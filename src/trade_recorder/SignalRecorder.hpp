#pragma once
// signal_recorder.hpp — SQLite-backed strategy signal journal (M32)
//
// Appends every raw strategy signal (the pre-aggregation stream) plus every
// aggregator consensus emission to a `signals` table in the same SQLite file
// as TradeRecorder — a third WAL connection, like MarketRecorder. This is the
// replay source for the BTC A/B weight trial: counterfactual aggregator
// weight policies are replayed offline over this journal.
//
// Signals are LOW frequency (strategy state changes, gated by per-strategy
// cooldowns) and variable-length (reason text, indicator snapshot JSON), so —
// unlike the ticker/kline hot path — they do not need a no-alloc POD ring.
// `record()` pushes into a bounded std::deque under a mutex; a dedicated
// writer thread flushes batches (BEGIN IMMEDIATE transactions) on a size or
// time threshold. Same single-writer invariant as MarketRecorder: m_db is
// only ever touched by the writer thread.
//
// Table:
//   signals — one row per journaled signal (id AUTOINCREMENT, indexed by
//             (strategy_id, ts_ns) and (symbol, ts_ns)). Append-only; the
//             engine never prunes it.
//
// Rows with strategy_id = "signal_aggregator" are consensus emissions and are
// EXCLUDED from replay inputs (the replay synthesizes its own aggregator);
// they exist so a later auto-trade run can cross-check replay fidelity.

#include "core/PulseError.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Forward-declare SQLite to avoid leaking it into the public header.
namespace SQLite
{
class Database;
}

namespace pulse::trade_recorder
{

// ---------------------------------------------------------------------------
// SignalRecord — one journaled signal (queue entry; strings are fine here,
// the hot ticker path is the no-alloc one, this is a strategy-thread path)
// ---------------------------------------------------------------------------
struct SignalRecord
{
    std::int64_t ts_ns;          ///< Signal timestamp as epoch nanoseconds.
    std::string strategy_id;     ///< e.g. "eth_scalper_BTC_USDT"; the literal
                                 ///< "signal_aggregator" marks consensus rows.
    std::string symbol;          ///< Trading pair (e.g. "BTC_USDT").
    std::string market_type;     ///< Wire form: "spot", "futures" or "cfd".
    std::string type;            ///< Wire form: "buy", "sell" or "flat".
    double confidence;           ///< Signal strength in [0.0, 1.0].
    double price;                ///< Reference price at generation time.
    std::string reason;          ///< Human-readable explanation.
    std::string indicators;      ///< Indicator snapshot as JSON text.
};

// ---------------------------------------------------------------------------
// SignalRecorder — SQLite signal journal
//
// Usage:
//   auto result = SignalRecorder::open("data/trades.db");
//   if (ok(result)) {
//       auto recorder = std::move(value(result));  // unique_ptr
//       recorder->record(sig);                     // from strategy threads
//       ...
//       recorder->stop();                          // drains + checkpoints
//   }
// ---------------------------------------------------------------------------
class SignalRecorder
{
  public:
    /// Open (or create) the database and start the writer thread. Third
    /// connection to the same file as TradeRecorder/MarketRecorder (WAL).
    /// Returns an error on SQLite failure.
    [[nodiscard]] static Result<std::unique_ptr<SignalRecorder>> open(
        const std::string &db_path,
        std::chrono::milliseconds flush_interval = std::chrono::milliseconds(1000));

    ~SignalRecorder();

    SignalRecorder(const SignalRecorder &) = delete;
    SignalRecorder &operator=(const SignalRecorder &) = delete;
    SignalRecorder(SignalRecorder &&) = delete;
    SignalRecorder &operator=(SignalRecorder &&) = delete;

    /// Enqueue one signal for persistence (O(1) amortized, never blocks).
    /// No-op after stop(). When the queue is full the OLDEST entry is
    /// dropped (bounded backlog — a stalled writer must not stall strategy
    /// threads).
    void record(const SignalRecord &rec);

    /// Stop the writer thread, drain + flush everything remaining,
    /// run wal_checkpoint(TRUNCATE) and close the connection.
    /// Idempotent — safe to call multiple times.
    void stop();

    /// Number of signals dropped because the queue was full.
    /// Diagnostic / test aid.
    [[nodiscard]] std::size_t droppedCount() const;

    /// Number of signals currently queued (not yet flushed).
    /// Diagnostic / test aid.
    [[nodiscard]] std::size_t queueSize() const;

  private:
    SignalRecorder(std::unique_ptr<SQLite::Database> db,
                   std::chrono::milliseconds flush_interval);

    /// Writer thread: flush batches (256 rows or flush_interval), then on
    /// shutdown drain everything and checkpoint the WAL.
    void writerLoop(std::stop_token stoken);

    /// Flush the whole current queue inside one BEGIN IMMEDIATE transaction.
    void flushQueue();

    std::unique_ptr<SQLite::Database> m_db;

    // --- Queue (producer: strategy threads; consumer: writer thread) ---
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<SignalRecord> m_queue;
    std::size_t m_droppedCount{ 0 };
    bool m_closing{ false };

    static constexpr std::size_t kMaxPending = 65536;
    static constexpr std::size_t kBatchSize = 256;

    std::chrono::milliseconds m_flushInterval;
    std::jthread m_writer;
};

} // namespace pulse::trade_recorder
