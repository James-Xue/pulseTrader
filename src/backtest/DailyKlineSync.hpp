#pragma once
// daily_kline_sync.hpp — Daily kline archival sync (M31)
//
// Gate REST serves at most the most-recent 10000 points per interval
// (measured 2026-08-23: "Candlestick too long ago. Maximum 10000 points
// recently are allowed"). Since M30 retired WS-based futures kline
// recording, local kline_bars history would otherwise stop growing — so the
// engine syncs once per day: pull ~10000 1m candles (spot/futures) + 500
// gold candles (CFD) via REST and merge them into kline_bars (INSERT OR
// IGNORE, idempotent).
//
// Scheduling:
//   - The main loop calls tick(now_ms) every 200ms. A pure day-boundary
//     check (same algorithm as GridManager::tickFast) fires at most once per
//     UTC day boundary (default Beijing 08:00 == UTC 00:00).
//   - The first tick after construction fires immediately (day_start starts
//     at 0), so a fresh start backfills history right away.
//   - The actual fetching runs on an internal worker thread — the main loop
//     only does an integer day-key check (µs), never blocks.
//
// Failure mode: per-symbol errors are WARN-logged and skipped; the next day
// retries. Never throws out of tick().

#include "backtest/KlineSource.hpp"
#include "core/types.hpp"
#include "exchange/GateRestClient.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// CfdKlineSource — IKlineSource adapter over Gate's TradFi klines endpoint.
// GateKlineFetcher rejects CFD, so this thin wrapper fills that gap.
// ---------------------------------------------------------------------------
class CfdKlineSource final : public IKlineSource
{
  public:
    explicit CfdKlineSource(exchange::GateRestClient &rest);

    Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override;

    std::string description() const override
    {
        return "cfd-rest";
    }

  private:
    exchange::GateRestClient &m_rest;
};

// ---------------------------------------------------------------------------
// DailyKlineSync — daily REST kline archival into kline_bars
// ---------------------------------------------------------------------------
class DailyKlineSync
{
  public:
    /// `sqlite` may be null (non-SQLite build → fetch-only, no persistence).
    /// `spot_api`/`futures_api` are the fetch sources (typically
    /// GateKlineFetcher wrapping the matching REST client). `cfd_api` may be
    /// null → CFD symbols are skipped.
    /// `rest_mutex` serialises the underlying REST calls (GateRestClient is
    /// not thread-safe; the worker thread races the CFD poll / control plane).
    DailyKlineSync(IKlineSource *sqlite,
                   IKlineSource &spot_api,
                   IKlineSource &futures_api,
                   IKlineSource *cfd_api,
                   std::mutex &rest_mutex,
                   std::vector<std::string> spot_symbols,
                   std::vector<std::string> futures_symbols,
                   std::vector<std::string> cfd_symbols,
                   int daily_hour);

    ~DailyKlineSync();

    DailyKlineSync(const DailyKlineSync &) = delete;
    DailyKlineSync &operator=(const DailyKlineSync &) = delete;
    DailyKlineSync(DailyKlineSync &&) = delete;
    DailyKlineSync &operator=(DailyKlineSync &&) = delete;

    /// Main-loop hook (200ms cadence). Day-boundary check + fire-once.
    /// Never blocks; the fetch runs on the worker thread.
    void tick(std::int64_t now_ms);

  private:
    void workerLoop(std::stop_token stoken);
    void syncOnce();
    void syncMarket(const std::vector<std::string> &symbols, MarketType mt,
                    IKlineSource &api, std::int64_t from_ms, std::int64_t to_ms);

    IKlineSource *m_sqlite;
    IKlineSource &m_spotApi;
    IKlineSource &m_futuresApi;
    IKlineSource *m_cfdApi;
    std::mutex &m_restMutex;
    std::vector<std::string> m_spotSymbols;
    std::vector<std::string> m_futuresSymbols;
    std::vector<std::string> m_cfdSymbols;
    int m_dailyHour;

    // --- worker / scheduling state ---
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::jthread m_worker;
    bool m_pending{ false };      ///< A sync has been requested.
    bool m_inFlight{ false };     ///< A sync is currently running.
    std::int64_t m_dayStartSec{ 0 }; ///< Last seen UTC day boundary (0 = first tick fires).
};

} // namespace pulse::backtest
