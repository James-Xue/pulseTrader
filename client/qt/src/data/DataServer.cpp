#include "data/DataServer.hpp"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonValue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVariantMap>

#include <utility>

#include "data/DataSession.hpp"
#include "data/MarketDataService.hpp"

namespace pulse::client::data
{

namespace
{

// Terminal rejection codes from DATA IPC PROTOCOL v1. A client that receives
// one of these is disconnected immediately afterwards.
constexpr int kErrDuplicateClientId = -32001;
constexpr int kErrServerFull = -32002;
constexpr int kErrInvalidHello = -32003;
constexpr int kErrUnsupportedProtocol = -32004;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QJsonValue idOrNull(const QJsonValue& id)
{
    return id.isUndefined() ? QJsonValue(QJsonValue::Null) : id;
}

/// A JSON-RPC success response.
QJsonObject makeResult(const QJsonValue& id, const QJsonObject& result)
{
    QJsonObject message;
    message[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    message[QStringLiteral("id")] = idOrNull(id);
    message[QStringLiteral("result")] = result;
    return message;
}

/// A JSON-RPC notification: no id, so the consumer never correlates it.
QJsonObject makeNotification(const QString& method, const QJsonObject& params)
{
    QJsonObject message;
    message[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    message[QStringLiteral("method")] = method;
    message[QStringLiteral("params")] = params;
    return message;
}

} // namespace

DataServer::DataServer(QObject* parent)
    : DataServer(DataServerConfig{}, parent)
{
}

DataServer::DataServer(const DataServerConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_tcpServer(new QTcpServer(this))
{
    connect(m_tcpServer, &QTcpServer::newConnection, this,
            &DataServer::onNewConnection);
}

DataServer::~DataServer()
{
    stop();
}

bool DataServer::start()
{
    if (m_tcpServer->isListening())
    {
        return true;
    }

    const QHostAddress address(m_config.bindAddress);
    if (!m_tcpServer->listen(address, m_config.port))
    {
        const QString message =
            QStringLiteral("data server: cannot listen on %1:%2 (%3)")
                .arg(m_config.bindAddress)
                .arg(m_config.port)
                .arg(m_tcpServer->errorString());
        emit errorOccurred(message);
        return false;
    }

    // One call brings up the whole pipeline: listener first, then the engine
    // poller. Tests set pollEngine = false and drive publish*() themselves, so
    // no background connection can race their assertions.
    if (m_config.pollEngine && nullptr == m_market)
    {
        m_market = new MarketDataService(m_config, this);
        connect(m_market, &MarketDataService::snapshotReady, this,
                &DataServer::publishSnapshot);
        connect(m_market, &MarketDataService::updateReady, this,
                &DataServer::publishUpdate);
        m_market->start();
    }

    emit listeningChanged();
    return true;
}

void DataServer::stop()
{
    if (nullptr != m_market)
    {
        m_market->stop();
    }

    // Copy first: closeSession() can emit closed synchronously for a socket
    // that is already unconnected, and onSessionClosed() mutates m_sessions.
    const QList<DataSession*> sessions = m_sessions;
    for (DataSession* session : sessions)
    {
        session->closeSession();
    }

    m_tcpServer->close();
    emit listeningChanged();
}

void DataServer::publishSnapshot(const QJsonObject& params)
{
    const QJsonObject stamped = stamp(params);

    // Cache it for the next hello. The cached copy is what a consumer that
    // connects between two polls receives, so it must be the stamped frame
    // (seq included) and not the raw producer payload.
    m_cachedSnapshot = stamped;
    applyEngineReady(stamped);
    broadcast(QStringLiteral("snapshot"), stamped);
}

void DataServer::publishUpdate(const QJsonObject& params)
{
    const QJsonObject stamped = stamp(params);
    applyEngineReady(stamped);
    broadcast(QStringLiteral("market.update"), stamped);
}

bool DataServer::listening() const
{
    return m_tcpServer->isListening();
}

int DataServer::port() const
{
    return m_tcpServer->isListening()
               ? static_cast<int>(m_tcpServer->serverPort())
               : static_cast<int>(m_config.port);
}

QVariantList DataServer::clients() const
{
    QVariantList list;
    for (const DataSession* session : m_sessions)
    {
        if (!session->welcomed())
        {
            continue;
        }
        QVariantMap entry;
        entry[QStringLiteral("client_id")] = session->clientId();
        entry[QStringLiteral("strategy")] = session->strategy();
        entry[QStringLiteral("connected_at")] = session->connectedAtMs();
        entry[QStringLiteral("last_seen_ms")] = session->lastSeenMs();
        list.append(entry);
    }
    return list;
}

void DataServer::onNewConnection()
{
    while (m_tcpServer->hasPendingConnections())
    {
        QTcpSocket* socket = m_tcpServer->nextPendingConnection();
        if (nullptr == socket)
        {
            break;
        }

        auto* session = new DataSession(socket, m_config, this);
        connect(session, &DataSession::helloReceived, this, &DataServer::onHello);
        connect(session, &DataSession::messageReceived, this,
                &DataServer::onSessionMessage);
        connect(session, &DataSession::closed, this, &DataServer::onSessionClosed);
        m_sessions.append(session);
    }
}

void DataServer::onHello(DataSession* session)
{
    const QJsonValue id = session->helloId();
    const DataSession::Hello hello = session->hello();

    // 1. One hello per socket. A second one means the consumer is confused or
    //    is trying to change identity mid-stream.
    if (session->welcomed())
    {
        session->reject(id, kErrInvalidHello,
                        QStringLiteral("invalid hello: already welcomed"));
        return;
    }

    // 2. Version before capacity: a consumer that speaks another protocol can
    //    never be served, so "server full" would be misleading.
    if (kProtocolVersion != hello.protocol)
    {
        session->reject(id, kErrUnsupportedProtocol,
                        QStringLiteral("unsupported protocol %1").arg(hello.protocol));
        return;
    }

    // 3. Capacity is counted over welcomed sessions only. A socket that has
    //    not said hello does not consume a seat.
    if (m_connectedCount >= m_config.maxClients)
    {
        session->reject(id, kErrServerFull, QStringLiteral("server full"));
        return;
    }

    // 4. Identity. Only welcomed sessions own an id, so a pending socket that
    //    used the same name does not block anybody.
    for (const DataSession* other : std::as_const(m_sessions))
    {
        if (other == session || !other->welcomed())
        {
            continue;
        }
        if (other->clientId() == hello.clientId)
        {
            session->reject(id, kErrDuplicateClientId,
                            QStringLiteral("duplicate client_id"));
            return;
        }
    }

    // 5. Accepted. Welcome first, snapshot second: the consumer learns the
    //    dataset shape before any payload arrives.
    session->acceptHello(hello);
    ++m_connectedCount;

    session->sendMessage(makeResult(id, welcome()));
    session->sendMessage(
        makeNotification(QStringLiteral("snapshot"), currentSnapshot()));

    emit connectedCountChanged();
    emit clientsChanged();
}

void DataServer::onSessionMessage(DataSession* session)
{
    Q_UNUSED(session);

    // last_seen_ms drives the QML client list; every frame (including a ping)
    // refreshes it, so the list re-evaluates on the next event loop turn.
    emit clientsChanged();
}

void DataServer::onSessionClosed(DataSession* session)
{
    if (!m_sessions.removeOne(session))
    {
        return;
    }

    const bool wasWelcomed = session->welcomed();
    if (wasWelcomed && m_connectedCount > 0)
    {
        --m_connectedCount;
    }

    session->deleteLater();

    emit clientsChanged();
    if (wasWelcomed)
    {
        emit connectedCountChanged();
    }
}

QJsonObject DataServer::welcome() const
{
    QJsonObject result;
    result[QStringLiteral("protocol")] = kProtocolVersion;
    result[QStringLiteral("server")] = QStringLiteral("pulseTrader");
    result[QStringLiteral("max_clients")] = m_config.maxClients;
    result[QStringLiteral("connected")] = m_connectedCount;
    result[QStringLiteral("engine_ready")] = m_engineReady;
    result[QStringLiteral("symbol")] = m_config.symbol;
    result[QStringLiteral("market_type")] = m_config.marketType;
    result[QStringLiteral("timeframe")] = m_config.timeframe;
    result[QStringLiteral("indicator_params")] = m_config.indicators.toJson();
    return result;
}

QJsonObject DataServer::currentSnapshot() const
{
    if (!m_cachedSnapshot.isEmpty())
    {
        return m_cachedSnapshot;
    }

    // No poll has succeeded yet. Sending the smallest well-formed frame is
    // better than silence: the consumer can start its parser immediately and
    // knows the server is alive but has no feed.
    QJsonObject snapshot;
    snapshot[QStringLiteral("seq")] = static_cast<double>(m_seq);
    snapshot[QStringLiteral("ts")] = static_cast<double>(nowMs());
    snapshot[QStringLiteral("engine_ready")] = m_engineReady;
    snapshot[QStringLiteral("symbol")] = m_config.symbol;
    snapshot[QStringLiteral("market_type")] = m_config.marketType;
    snapshot[QStringLiteral("timeframe")] = m_config.timeframe;
    snapshot[QStringLiteral("ticker")] = QJsonObject{};
    snapshot[QStringLiteral("klines")] = QJsonArray{};
    snapshot[QStringLiteral("indicators")] = QJsonObject{};
    return snapshot;
}

QJsonObject DataServer::stamp(const QJsonObject& params)
{
    // The server is the single sequencer. A producer-side seq is deliberately
    // overwritten: two produce paths (the engine poller and a direct publish
    // from a test or tool) must not interleave two counters, and every client
    // has to see the same number for the same frame.
    QJsonObject stamped = params;
    stamped[QStringLiteral("seq")] = static_cast<double>(++m_seq);
    if (!stamped.contains(QStringLiteral("ts")))
    {
        stamped[QStringLiteral("ts")] = static_cast<double>(nowMs());
    }
    return stamped;
}

void DataServer::broadcast(const QString& method, const QJsonObject& params)
{
    const QJsonObject message = makeNotification(method, params);

    // Iterate a copy: sendMessage() may close a slow consumer, and a close can
    // reach onSessionClosed() before this loop ends.
    const QList<DataSession*> sessions = m_sessions;
    for (DataSession* session : sessions)
    {
        if (!session->welcomed())
        {
            continue;
        }
        session->sendMessage(message);
    }
}

void DataServer::applyEngineReady(const QJsonObject& params)
{
    if (!params.contains(QStringLiteral("engine_ready")))
    {
        return;
    }
    setEngineReady(params.value(QStringLiteral("engine_ready")).toBool());
}

void DataServer::setEngineReady(bool ready)
{
    if (ready == m_engineReady)
    {
        return;
    }
    m_engineReady = ready;
    emit engineReadyChanged();
}

} // namespace pulse::client::data
