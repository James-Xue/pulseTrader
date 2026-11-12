#include "market/MarketStream.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>

#include "rpc/RpcMethods.hpp"

namespace pulse::client
{

namespace
{

/// Local wall-clock milliseconds. Buckets are aligned to this clock, not to
/// the exchange timestamp: the chart answers "what happened during the last N
/// seconds on this machine", and a skewed exchange clock must not move bars.
qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

/// Read a price field. The engine serialises every numeric field as a double,
/// but a hand-written test reply may omit it, so a missing key becomes 0.0.
double priceField(const QJsonObject& object, const QString& key)
{
    return object.value(key).toDouble(0.0);
}

} // namespace

MarketStream::MarketStream(QObject* parent)
    : QObject(parent)
    , m_rpc(new RpcClient(QStringLiteral("market-stream"), this))
    , m_aggregator(static_cast<qint64>(kDefaultCandlePeriodSec) * 1000, kMaxCandles)
{
    // 1. Reply routing is queued so a synchronous failure from call() (a short
    //    write) cannot run before m_inFlightId has been assigned.
    connect(m_rpc, &RpcClient::callSucceeded, this,
            [this](qint64 id, const QJsonObject& result)
    {
        QMetaObject::invokeMethod(this, [this, id, result]()
        {
            onCallSucceeded(id, result);
        }, Qt::QueuedConnection);
    });
    connect(m_rpc, &RpcClient::callFailed, this,
            [this](qint64 id, const RpcError& error)
    {
        QMetaObject::invokeMethod(this, [this, id, error]()
        {
            onCallFailed(id, error);
        }, Qt::QueuedConnection);
    });

    // 2a. A rejected token is terminal until the endpoint is reconfigured.
    //     Surface it instead of looping on the same failed handshake.
    connect(m_rpc, &RpcClient::authenticationFailed, this,
            [this](const QString& message)
    {
        setEngineReady(false);
        setStale(true);
        setLastError(QStringLiteral("auth: ") + message);
    });

    // 2. A socket that becomes Ready after start() must poll immediately
    //    instead of waiting for the next timer tick.
    connect(m_rpc, &RpcClient::stateChanged, this, [this](RpcClient::State state)
    {
        if (RpcClient::State::Ready == state && m_streaming)
        {
            poll();
        }
    });

    // 3. PreciseTimer keeps the one-sample-per-second cadence honest; the
    //    default coarse timer can drift by a few percent, which would make a
    //    10 s candle occasionally hold 9 or 11 samples.
    m_pollTimer.setTimerType(Qt::PreciseTimer);
    m_pollTimer.setInterval(m_pollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &MarketStream::poll);
}

MarketStream::~MarketStream()
{
    stop();
}

void MarketStream::setEndpoint(const QString& host, quint16 port, bool tls,
                               const QString& token)
{
    m_rpc->setEndpoint(host, port, tls);
    m_rpc->setToken(token);
}

void MarketStream::start(const QString& symbol, const QString& marketType)
{
    const QString normalized = symbol.trimmed();
    if (normalized.isEmpty())
    {
        setLastError(QStringLiteral("symbol is required"));
        return;
    }

    // 1. Restarting the same instrument is a resume, not a reset. Leaving the
    //    Market page and coming back, or pressing Stop then Start, must keep
    //    the accumulated chart: the username explicitly stopped, they did not
    //    ask to discard up to 20 minutes of candles.
    if (normalized == m_symbol && marketType == m_marketType
        && !m_symbol.isEmpty())
    {
        if (!m_streaming)
        {
            m_streaming = true;
            emit streamingChanged();
        }
        setLastError(QString());
        setStale(true);

        if (RpcClient::State::Disconnected == m_rpc->state())
        {
            m_rpc->connectToEngine();
        }
        m_pollTimer.start();
        poll();
        return;
    }

    // 2. New symbol/market: every value belongs to the previous instrument,
    //    so clear it before the first reply arrives.
    m_symbol = normalized;
    m_marketType = marketType;
    emit symbolChanged();
    emit marketTypeChanged();

    m_lastPrice = 0.0;
    m_bid = 0.0;
    m_ask = 0.0;
    m_changePct = 0.0;
    m_volume24h = 0.0;
    m_tickerTimestamp = 0;
    m_rawTicker.clear();
    m_sampleCount = 0;
    m_lastSampleMs = 0;
    m_aggregator.clear();
    m_candles.clear();
    m_inFlight = false;
    m_inFlightId = -1;

    setEngineReady(false);
    setStale(true);
    setLastError(QString());
    emit tickerChanged();
    emit sampleChanged();
    emit candlesChanged();

    m_streaming = true;
    emit streamingChanged();

    // 3. Connect lazily; stop() closes the socket again. A client that never
    //    opens the Market page therefore never opens a fourth connection.
    if (RpcClient::State::Disconnected == m_rpc->state())
    {
        m_rpc->connectToEngine();
    }

    m_pollTimer.start();
    poll();
}

void MarketStream::stop()
{
    m_pollTimer.stop();
    m_inFlight = false;
    m_inFlightId = -1;

    if (m_streaming)
    {
        m_streaming = false;
        emit streamingChanged();
    }

    m_rpc->disconnectFromEngine();
    setEngineReady(false);
    setStale(true);
    // A stop is a deliberate action, so the last transport error is no longer
    // relevant; leaving it on screen reads as a live fault.
    setLastError(QString());
}

void MarketStream::clearCandles()
{
    m_aggregator.clear();
    m_candles.clear();
    m_sampleCount = 0;
    m_lastSampleMs = 0;
    emit candlesChanged();
    emit sampleChanged();
}

void MarketStream::setCandlePeriodSec(int seconds)
{
    const int clamped = qBound(1, seconds, 3600);
    if (clamped == m_candlePeriodSec)
    {
        return;
    }

    m_candlePeriodSec = clamped;
    m_aggregator.setPeriodMs(static_cast<qint64>(clamped) * 1000);
    m_candles.clear();
    m_sampleCount = 0;
    m_lastSampleMs = 0;
    emit candlePeriodSecChanged();
    emit candlesChanged();
    emit sampleChanged();
}

void MarketStream::setPollIntervalMs(int milliseconds)
{
    const int clamped = qBound(kMinPollIntervalMs, milliseconds, kMaxPollIntervalMs);
    if (clamped == m_pollIntervalMs)
    {
        return;
    }

    m_pollIntervalMs = clamped;
    m_pollTimer.setInterval(clamped);
    emit pollIntervalMsChanged();
}

void MarketStream::poll()
{
    // 1. Age the last sample before the transport check, so an engine that
    //    silently stops replying still flips the stale badge.
    refreshStale();

    if (!m_streaming || m_inFlight)
    {
        return;
    }

    // 2. An unavailable socket is reported as "not ready", not as an error;
    //    RpcClient reconnects on its own backoff.
    if (RpcClient::State::Ready != m_rpc->state())
    {
        setEngineReady(false);
        setStale(true);
        return;
    }

    // 3. Only the ticker matters for this page. book_levels = 0 keeps the
    //    order book empty and klines = 1 gives a fallback close if a feed has
    //    not published a ticker yet.
    QJsonObject params;
    params[QStringLiteral("symbol")] = m_symbol;
    params[QStringLiteral("book_levels")] = 0;
    params[QStringLiteral("klines")] = 1;
    if (!m_marketType.isEmpty())
    {
        params[QStringLiteral("market_type")] = m_marketType;
    }

    m_inFlight = true;
    m_inFlightId = m_rpc->call(methods::kGetMarket, params, kCallTimeoutMs);
}

void MarketStream::onCallSucceeded(qint64 id, const QJsonObject& result)
{
    if (id != m_inFlightId || !m_streaming)
    {
        return;
    }
    m_inFlight = false;
    m_inFlightId = -1;

    // 1. The engine reports logical failures as a top-level "error" string
    //    inside a successful JSON-RPC reply.
    if (result.contains(QStringLiteral("error")))
    {
        setEngineReady(false);
        setStale(true);
        setLastError(result.value(QStringLiteral("error")).toString());
        return;
    }

    const QJsonObject ticker = result.value(QStringLiteral("ticker")).toObject();
    const QJsonArray klines = result.value(QStringLiteral("klines")).toArray();
    applyTicker(ticker, klines);
}

void MarketStream::onCallFailed(qint64 id, const RpcError& error)
{
    if (id != m_inFlightId)
    {
        return;
    }
    m_inFlight = false;
    m_inFlightId = -1;

    setEngineReady(false);
    setStale(true);
    // Preserve the transport kind and message: a 2 s timeout, a protocol
    // desync and a refused socket are three different user actions.
    setLastError(error.toString());
}

void MarketStream::applyTicker(const QJsonObject& ticker,
                               const QJsonArray& klines)
{
    // 1. Resolve a display price. The ticker is authoritative; the newest kline
    //    close is a display-only fallback for a feed that has not published a
    //    ticker yet. Flag which one supplied the value: only a genuine ticker
    //    may enter the aggregator.
    double price = 0.0;
    bool fromTicker = false;
    if (!ticker.isEmpty())
    {
        price = priceField(ticker, QStringLiteral("last"));
        if (0.0 >= price)
        {
            price = priceField(ticker, QStringLiteral("bid"));
        }
        if (0.0 >= price)
        {
            price = priceField(ticker, QStringLiteral("ask"));
        }
        fromTicker = 0.0 < price;
    }
    if (0.0 >= price && !klines.isEmpty())
    {
        price = priceField(klines.last().toObject(), QStringLiteral("close"));
    }

    if (0.0 >= price)
    {
        setEngineReady(false);
        setStale(true);
        setLastError(QStringLiteral("no price for ") + m_symbol);
        return;
    }

    const qint64 now = nowMs();
    const qint64 exchangeTimestamp =
        static_cast<qint64>(priceField(ticker, QStringLiteral("timestamp")));

    // 2. Display the newest values even when the exchange timestamp is old;
    //    the stale badge explains why the chart is not moving.
    m_lastPrice = price;
    m_bid = priceField(ticker, QStringLiteral("bid"));
    m_ask = priceField(ticker, QStringLiteral("ask"));
    m_changePct = priceField(ticker, QStringLiteral("change_pct"));
    m_volume24h = priceField(ticker, QStringLiteral("volume_24h"));
    m_tickerTimestamp = exchangeTimestamp;
    m_rawTicker = ticker.toVariantMap();
    setEngineReady(true);
    setLastError(QString());
    emit tickerChanged();

    // 3. Fold only a genuine, fresh ticker.
    //    a. A kline fallback is a 1-minute close: folding it once per second
    //       would draw a flat candle series that never traded.
    //    b. A ticker without a timestamp cannot be proven fresh. The engine
    //       stamps every WS/REST ticker with receive time, so a zero means the
    //       feed did not supply one; treating it as live is a false positive.
    //    c. An explicitly old ticker is also skipped and flagged stale.
    if (!fromTicker || 0 >= exchangeTimestamp)
    {
        setStale(true);
        return;
    }
    if (now - exchangeTimestamp > kStaleAfterMs)
    {
        setStale(true);
        return;
    }

    if (!m_aggregator.addSample(price, now))
    {
        setStale(true);
        return;
    }

    ++m_sampleCount;
    m_lastSampleMs = now;
    setStale(false);
    rebuildCandles();
    emit sampleChanged();
    emit candlesChanged();
}

void MarketStream::rebuildCandles()
{
    QVariantList list;
    const QList<Candle>& candles = m_aggregator.candles();
    list.reserve(candles.size());

    for (const Candle& candle : candles)
    {
        QVariantMap entry;
        entry[QStringLiteral("t")] = static_cast<double>(candle.bucket_start_ms);
        entry[QStringLiteral("o")] = candle.open;
        entry[QStringLiteral("h")] = candle.high;
        entry[QStringLiteral("l")] = candle.low;
        entry[QStringLiteral("c")] = candle.close;
        entry[QStringLiteral("n")] = candle.samples;
        entry[QStringLiteral("closed")] = candle.closed;
        list.append(entry);
    }

    m_candles = list;
}

void MarketStream::refreshStale()
{
    if (!m_engineReady)
    {
        setStale(true);
        return;
    }

    if (0 < m_lastSampleMs
        && (nowMs() - m_lastSampleMs) > (3LL * m_pollIntervalMs))
    {
        setStale(true);
    }
}

void MarketStream::setEngineReady(bool ready)
{
    if (ready == m_engineReady)
    {
        return;
    }
    m_engineReady = ready;
    emit engineReadyChanged();
}

void MarketStream::setStale(bool stale)
{
    if (stale == m_stale)
    {
        return;
    }
    m_stale = stale;
    emit staleChanged();
}

void MarketStream::setLastError(const QString& message)
{
    if (message == m_lastError)
    {
        return;
    }
    m_lastError = message;
    emit lastErrorChanged();
}

} // namespace pulse::client