// daily_kline_sync.cpp — DailyKlineSync implementation (M31)

#include "backtest/DailyKlineSync.hpp"

#include "logging/Logger.hpp"
#include "market/MarketFeed.hpp"

#include <chrono>

namespace pulse::backtest
{

namespace
{

/// 1m bar length in ms; Gate REST serves the most-recent 10000 points.
constexpr std::int64_t kMinuteMs = 60'000;
/// Futures window: 9995 not 10000 — the "most-recent N points" boundary is
/// DYNAMIC (re-checked against the server clock at request time), so a
/// window starting at now-10000min fails seconds later. 5 minutes of slack
/// absorbs clock skew + the whole market's fetch time; the oldest minutes
/// are re-covered by earlier local rows (idempotent INSERT OR IGNORE merge).
constexpr std::int64_t kFuturesPoints = 9'995;
/// Spot serves at most ~9999 points of history (probed 2026-08-23: a from at
/// now-9999min is rejected with "too long ago"), so give it 10 minutes.
constexpr std::int64_t kSpotPoints = 9'990;
/// Retry window: on failure, shift the window 10 minutes forward (away from
/// the dynamic boundary) and try once more before giving up for the day.
constexpr std::int64_t kRetrySlackMin = 10;

} // anonymous namespace

// ---------------------------------------------------------------------------
// CfdKlineSource
// ---------------------------------------------------------------------------

CfdKlineSource::CfdKlineSource(exchange::GateRestClient &rest)
    : m_rest{ rest }
{
}

Result<std::vector<market::Kline>> CfdKlineSource::fetch(
    const std::string &symbol, MarketType market_type,
    std::int64_t /*from_ms*/, std::int64_t /*to_ms*/)
{
    if (MarketType::Cfd != market_type)
    {
        return PulseError{ ErrorCode::ConfigInvalidValue,
                           "CfdKlineSource only serves CFD symbols" };
    }

    auto raw = m_rest.getCfdKlines(symbol, 500);
    if (!ok(raw))
    {
        return error(raw);
    }

    std::vector<market::Kline> out;
    for (const auto &obj : value(raw))
    {
        if (auto k = market::MarketFeed::parseCfdKline(obj))
        {
            out.push_back(*k);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// DailyKlineSync
// ---------------------------------------------------------------------------

DailyKlineSync::DailyKlineSync(IKlineSource *sqlite,
                               IKlineSource &spot_api,
                               IKlineSource &futures_api,
                               IKlineSource *cfd_api,
                               std::mutex &rest_mutex,
                               std::vector<std::string> spot_symbols,
                               std::vector<std::string> futures_symbols,
                               std::vector<std::string> cfd_symbols,
                               int daily_hour)
    : m_sqlite{ sqlite }
    , m_spotApi{ spot_api }
    , m_futuresApi{ futures_api }
    , m_cfdApi{ cfd_api }
    , m_restMutex{ rest_mutex }
    , m_spotSymbols{ std::move(spot_symbols) }
    , m_futuresSymbols{ std::move(futures_symbols) }
    , m_cfdSymbols{ std::move(cfd_symbols) }
    , m_dailyHour{ daily_hour }
{
    m_worker = std::jthread([this](std::stop_token st) { workerLoop(std::move(st)); });
}

DailyKlineSync::~DailyKlineSync()
{
    {
        std::lock_guard lock(m_mutex);
        m_pending = false; // Let the worker exit without another fetch.
    }
    m_cv.notify_all();
    if (m_worker.joinable())
    {
        m_worker.request_stop();
        m_worker.join();
    }
}

void DailyKlineSync::tick(std::int64_t now_ms)
{
    // Same day-key algorithm as GridManager::tickFast: the configured hour is
    // Beijing time, UTC+8, so shift by (hour - 8) before the day division.
    const double shift_h = static_cast<double>(m_dailyHour - 8);
    const std::int64_t now_sec = now_ms / 1000;
    const std::int64_t day_key = now_sec + static_cast<std::int64_t>(shift_h * 3600.0);
    const std::int64_t day_start = (day_key / 86400) * 86400;

    bool fire = false;
    {
        std::lock_guard lock(m_mutex);
        // Day boundary crossed (first tick counts: m_dayStartSec starts at 0)
        // and no sync is in flight. If one IS in flight, leave the boundary
        // un-consumed so the next tick re-fires right after it finishes.
        if (day_start != m_dayStartSec && !m_inFlight)
        {
            m_dayStartSec = day_start;
            m_pending = true;
            fire = true;
        }
    }
    if (fire)
    {
        m_cv.notify_one();
    }
}

void DailyKlineSync::workerLoop(std::stop_token stoken)
{
    for (;;)
    {
        bool should_run = false;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this, &stoken]
            {
                return m_pending || stoken.stop_requested();
            });
            if (stoken.stop_requested())
            {
                break;
            }
            should_run = m_pending;
            m_pending = false;
            m_inFlight = true;
        }

        if (should_run)
        {
            syncOnce();
            {
                std::lock_guard lock(m_mutex);
                m_inFlight = false;
            }
            m_cv.notify_all(); // Wake any tick waiting on inFlight.
        }
    }
}

void DailyKlineSync::syncOnce()
{
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // Spot serves slightly less history than futures (probed 2026-08-23), so
    // its window gets more slack.
    const auto futures_from = now_ms - kFuturesPoints * kMinuteMs;
    const auto spot_from = now_ms - kSpotPoints * kMinuteMs;

    PULSE_LOG_INFO("kline_sync", "daily sync start — pulling last {} (spot) / {} (futures) min candles",
                   kSpotPoints, kFuturesPoints);

    syncMarket(m_spotSymbols, MarketType::Spot, m_spotApi, spot_from, now_ms);
    syncMarket(m_futuresSymbols, MarketType::Futures, m_futuresApi, futures_from, now_ms);
    if (m_cfdApi)
    {
        syncMarket(m_cfdSymbols, MarketType::Cfd, *m_cfdApi, futures_from, now_ms);
    }

    PULSE_LOG_INFO("kline_sync", "daily sync done (spot {} futures {} cfd {} symbols)",
                   m_spotSymbols.size(), m_futuresSymbols.size(),
                   m_cfdApi ? m_cfdSymbols.size() : 0u);
}

void DailyKlineSync::syncMarket(const std::vector<std::string> &symbols,
                                MarketType mt, IKlineSource &api,
                                std::int64_t from_ms, std::int64_t to_ms)
{
    if (symbols.empty())
    {
        return;
    }

    for (const auto &symbol : symbols)
    {
        // Hold the shared REST mutex for the whole symbol's fetch: the
        // worker thread races the CFD poll thread and control-plane REST.
        // Per-symbol granularity (~1-3s) matches the CFD poll loop's
        // per-request locking and keeps other REST users waiting briefly.
        std::unique_lock rest_lock(m_restMutex);
        auto candles = api.fetch(symbol, mt, from_ms, to_ms);
        rest_lock.unlock();

        // Retry once with the window shifted forward: the dynamic "most-
        // recent N points" boundary can move past our from_ms between the
        // first and last symbols of a market (fetch time + clock skew), so a
        // later retry with a younger from succeeds where the first failed.
        if (!ok(candles))
        {
            const auto retry_from = from_ms + kRetrySlackMin * kMinuteMs;
            std::unique_lock retry_lock(m_restMutex);
            candles = api.fetch(symbol, mt, retry_from, to_ms);
            retry_lock.unlock();
            if (!ok(candles))
            {
                PULSE_LOG_WARN("kline_sync", "[{}] sync failed after retry ({}): skipping, retries next day",
                               symbol, error(candles).message);
                continue;
            }
        }

        const auto rows = value(candles);
        if (rows.empty())
        {
            PULSE_LOG_INFO("kline_sync", "[{}] sync: no candles in window", symbol);
            continue;
        }

        std::size_t written = 0;
        if (m_sqlite)
        {
            auto w = m_sqlite->writeBack(symbol, mt, rows);
            if (ok(w))
            {
                written = value(w);
            }
            else
            {
                PULSE_LOG_WARN("kline_sync", "[{}] writeBack failed ({}): data not persisted",
                               symbol, error(w).message);
            }
        }

        PULSE_LOG_INFO("kline_sync", "[{}] synced {} candles ({} new rows in kline_bars)",
                       symbol, rows.size(), written);
    }
}

} // namespace pulse::backtest
