#include "data/MarketDataService.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>

#include "data/IndicatorEngine.hpp"
#include "rpc/RpcMethods.hpp"

namespace pulse::client::data
{

namespace
{

/// Per-call deadline for get_market. The poll interval is 500 ms, so a reply
/// that takes longer than four intervals is already useless: the next poll
/// would overwrite it.
constexpr int kMarketTimeoutMs = 2000;

/// A ticker older than this is republished with stale = true. The engine
/// refreshes tickers continuously, so five seconds of silence means the feed,
/// not the network, is behind.
constexpr qint64 kStaleAfterMs = 5000;

/// Refresh the cached 500-candle snapshot every 60 successful polls (about
/// 30 s). A consumer that connects later therefore gets a recent baseline
/// instead of the one captured when the server started.
constexpr int kSnapshotRefreshPolls = 60;

/// QTimer uses coarse timers by default and may fire a few milliseconds early.
/// Without this slack a strict ">= interval" check would drop every second
/// frame and halve the stream to 1 Hz.
constexpr qint64 kRateLimitToleranceMs = 25;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

MarketDataService::MarketDataService(const DataServerConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_rpc(new RpcClient(QStringLiteral("data-server"), this))
{
    // 1. Loopback control socket, plain TCP. The engine on 127.0.0.1:8081
    //    accepts an unauthenticated first line, so no token is configured.
    m_rpc->setEndpoint(m_config.engineHost, m_config.enginePort, false);

    // 2. Reply routing. Both handlers are queued: RpcClient can emit a failure
    //    synchronously from inside call() (a short write), and handling it
    //    there would run before m_inFlightId has been assigned.
    connect(m_rpc, &RpcClient::callSucceeded, this,
            [this](qint64 id, const QJsonObject& result)
    {
        QMetaObject::invokeMethod(this, [this, id, result]()
        {
            onCallSucceeded(id, result);
        }, Qt::QueuedConnection);
    });
    connect(m_rpc, &RpcClient::callFailed, this, [this](qint64 id, const RpcError&)
    {
        QMetaObject::invokeMethod(this, [this, id]()
        {
            onCallFailed(id);
        }, Qt::QueuedConnection);
    });

    // 3. Poll immediately when the socket becomes Ready. Waiting for the next
    //    timer tick would delay the first snapshot after every engine restart.
    connect(m_rpc, &RpcClient::stateChanged, this, [this](RpcClient::State state)
    {
        if (RpcClient::State::Ready == state)
        {
            poll();
        }
    });

    m_pollTimer.setInterval(m_config.pollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &MarketDataService::poll);
}

MarketDataService::~MarketDataService()
{
    stop();
}

void MarketDataService::start()
{
    m_rpc->connectToEngine();
    m_pollTimer.start();

    // The first poll usually runs before the socket is Ready; poll() reports
    // the engine as unavailable, which is accurate at that instant.
    poll();
}

void MarketDataService::stop()
{
    m_pollTimer.stop();
    m_rpc->disconnectFromEngine();
}

void MarketDataService::poll()
{
    // 1. One request in flight at a time.
    if (m_inFlight)
    {
        return;
    }

    // 2. A down control socket is reported as "engine unavailable" rather than
    //    as an error: RpcClient reconnects on its own backoff.
    if (RpcClient::State::Ready != m_rpc->state())
    {
        setEngineReady(false);
        publishUpdate(true);
        return;
    }

    // 3. The engine reads these exact named fields (see JsonRpcServer's
    //    get_market registration); book_levels = 1 is the cheapest value the
    //    handler accepts.
    QJsonObject params;
    params[QStringLiteral("symbol")] = m_config.symbol;
    params[QStringLiteral("market_type")] = m_config.marketType;
    params[QStringLiteral("klines")] = m_config.klineCount;
    params[QStringLiteral("book_levels")] = 1;

    m_inFlight = true;
    m_inFlightId = m_rpc->call(methods::kGetMarket, params, kMarketTimeoutMs);
}

void MarketDataService::onCallSucceeded(qint64 id, const QJsonObject& result)
{
    if (id != m_inFlightId)
    {
        return;
    }
    m_inFlight = false;
    m_inFlightId = -1;
    onMarketResult(result);
}

void MarketDataService::onCallFailed(qint64 id)
{
    if (id != m_inFlightId)
    {
        return;
    }
    m_inFlight = false;
    m_inFlightId = -1;

    setEngineReady(false);
    publishUpdate(true);
}

void MarketDataService::onMarketResult(const QJsonObject& result)
{
    // 1. The engine reports an unusable feed as a top-level "error" string
    //    (unknown market_type, no feed attached, ...). A reply without any
    //    ticker and without klines is equally unusable.
    const bool hasError = result.contains(QStringLiteral("error"));
    const QJsonObject ticker = result.value(QStringLiteral("ticker")).toObject();
    const QJsonArray klines = result.value(QStringLiteral("klines")).toArray();
    const bool ready = !hasError && (!ticker.isEmpty() || !klines.isEmpty());

    const bool recovered = ready && !m_engineReady;
    setEngineReady(ready);

    if (!ready)
    {
        publishUpdate(true);
        return;
    }

    // 2. Refresh the caches. Indicators only make sense over klines, so they
    //    keep their previous value when a reply carries none.
    if (!ticker.isEmpty())
    {
        m_lastTicker = ticker;
    }
    if (!klines.isEmpty())
    {
        // The newest candle drives the 2 Hz stream. It is usually the live,
        // still-open candle; the consumer tells them apart by "closed".
        m_lastKline = klines.last().toObject();

        const IndicatorEngine engine(IndicatorEngine::Params{
            m_config.indicators.emaPeriods,
            m_config.indicators.macdFast,
            m_config.indicators.macdSlow,
            m_config.indicators.macdSignal,
            m_config.indicators.atrPeriod,
            m_config.indicators.supertrendMultiplier });
        m_lastIndicators = engine.compute(klines);
    }

    ++m_successfulPolls;

    // 3. Snapshot: first success, every recovery, and a periodic refresh so a
    //    consumer connecting later does not receive a stale baseline.
    const bool dueForRefresh = m_latestSnapshot.isEmpty()
                               || recovered
                               || (0 == (m_successfulPolls % kSnapshotRefreshPolls));
    if (dueForRefresh)
    {
        QJsonObject snapshot;
        snapshot[QStringLiteral("seq")] = static_cast<double>(++m_seq);
        snapshot[QStringLiteral("ts")] = static_cast<double>(nowMs());
        snapshot[QStringLiteral("engine_ready")] = true;
        snapshot[QStringLiteral("symbol")] = m_config.symbol;
        snapshot[QStringLiteral("market_type")] = m_config.marketType;
        snapshot[QStringLiteral("timeframe")] = m_config.timeframe;
        snapshot[QStringLiteral("ticker")] = m_lastTicker;
        snapshot[QStringLiteral("klines")] = klines;
        snapshot[QStringLiteral("indicators")] = m_lastIndicators;
        m_latestSnapshot = snapshot;
        emit snapshotReady(snapshot);
    }

    // 4. Update: one light frame per poll, at most 2 Hz at the default cadence.
    publishUpdate(false);
}

void MarketDataService::publishUpdate(bool forceStale)
{
    const qint64 now = nowMs();

    // Rate limit: never more than one update per poll interval. The poll timer
    // alone would be enough at steady state, but the "socket became Ready"
    // trigger polls immediately and would otherwise add an extra frame.
    if (m_lastUpdateMs > 0
        && (now - m_lastUpdateMs) < (m_config.pollIntervalMs - kRateLimitToleranceMs))
    {
        return;
    }
    m_lastUpdateMs = now;

    QJsonObject update;
    update[QStringLiteral("seq")] = static_cast<double>(++m_seq);
    update[QStringLiteral("ts")] = static_cast<double>(now);
    update[QStringLiteral("engine_ready")] = m_engineReady;
    // Keep the frame shape stable: an unavailable engine sends empty objects
    // instead of dropping keys, so a consumer never has to test for presence.
    update[QStringLiteral("ticker")] = m_lastTicker;
    update[QStringLiteral("kline")] = m_lastKline;
    update[QStringLiteral("indicators")] = m_lastIndicators;
    update[QStringLiteral("stale")] = forceStale || isStale();
    emit updateReady(update);
}

bool MarketDataService::isStale() const
{
    if (!m_engineReady)
    {
        return true;
    }

    // A ticker without a timestamp cannot be aged; treat it as fresh rather
    // than flapping the stale flag on every frame.
    const double timestamp =
        m_lastTicker.value(QStringLiteral("timestamp")).toDouble(0.0);
    if (timestamp <= 0.0)
    {
        return false;
    }

    return (static_cast<double>(nowMs()) - timestamp) > static_cast<double>(kStaleAfterMs);
}

void MarketDataService::setEngineReady(bool ready)
{
    if (ready == m_engineReady)
    {
        return;
    }
    m_engineReady = ready;
    emit engineReadyChanged(ready);
}

} // namespace pulse::client::data
