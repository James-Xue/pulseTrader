#include "data/DataSession.hpp"

#include <QAbstractSocket>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonValue>
#include <QTcpSocket>

#include <cmath>

namespace pulse::client::data
{

namespace
{

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

DataSession::DataSession(QTcpSocket* socket, const DataServerConfig& config,
                         QObject* parent)
    : QObject(parent)
    , m_socket(socket)
    , m_config(config)
{
    m_socket->setParent(this);
    m_lastSeenMs = nowMs();

    connect(m_socket, &QTcpSocket::readyRead, this, &DataSession::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this,
            &DataSession::onSocketDisconnected);

    // 1. Idle watchdog. The consumer pings every 5 s, so 15 s of silence means
    //    three missed pings and the session is dead weight.
    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(m_config.clientIdleTimeoutMs);
    connect(&m_idleTimer, &QTimer::timeout, this, &DataSession::onIdleTimeout);
    m_idleTimer.start();

    // 2. Flush watchdog. disconnectFromHost() waits for queued bytes; a peer
    //    that stopped reading would hold those bytes (and this object) forever.
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(kFlushGraceMs);
    connect(&m_flushTimer, &QTimer::timeout, this, [this]()
    {
        if (nullptr != m_socket)
        {
            m_socket->abort();
        }
        emitClosedOnce();
    });
}

DataSession::~DataSession()
{
    if (nullptr != m_socket)
    {
        m_socket->disconnect(this);
    }
}

void DataSession::sendMessage(const QJsonObject& message)
{
    if (nullptr == m_socket || m_closing)
    {
        return;
    }
    if (QAbstractSocket::ConnectedState != m_socket->state())
    {
        return;
    }

    // One compact object per line, exactly the framing the consumer expects.
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    m_socket->write(line);

    // Slow consumer: 256 KiB that cannot drain means the peer is not reading.
    // Dropping the session beats growing the write buffer without bound; the
    // consumer is expected to reconnect.
    if (m_socket->bytesToWrite() > m_config.maxBackpressureBytes)
    {
        closeSession();
    }
}

void DataSession::reject(const QJsonValue& id, int code, const QString& message)
{
    QJsonObject error;
    error[QStringLiteral("code")] = code;
    error[QStringLiteral("message")] = message;

    QJsonObject envelope;
    envelope[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    envelope[QStringLiteral("id")] =
        id.isUndefined() ? QJsonValue(QJsonValue::Null) : id;
    envelope[QStringLiteral("error")] = error;

    sendMessage(envelope);
    closeSession();
}

void DataSession::closeSession()
{
    if (m_closing)
    {
        return;
    }
    m_closing = true;
    m_idleTimer.stop();

    if (nullptr == m_socket
        || QAbstractSocket::UnconnectedState == m_socket->state())
    {
        emitClosedOnce();
        return;
    }

    m_flushTimer.start();
    m_socket->disconnectFromHost();
}

void DataSession::acceptHello(const Hello& hello)
{
    m_hello = hello;
    m_welcomed = true;
    m_connectedAtMs = nowMs();
    m_lastSeenMs = m_connectedAtMs;
}

void DataSession::onReadyRead()
{
    if (nullptr == m_socket)
    {
        return;
    }

    m_buffer.append(m_socket->readAll());

    // 1. Split on '\n'. A '\r' before it is tolerated so a client that writes
    //    CRLF still parses.
    int newline = m_buffer.indexOf('\n');
    while (newline >= 0 && !m_closing)
    {
        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.endsWith('\r'))
        {
            line.chop(1);
        }
        if (!line.isEmpty())
        {
            handleLine(line);
        }
        newline = m_buffer.indexOf('\n');
    }

    // 2. A peer that streams without a delimiter is broken or hostile. Drop it
    //    rather than letting the buffer grow.
    if (m_buffer.size() > kMaxFrameBytes)
    {
        closeSession();
    }
}

void DataSession::handleLine(const QByteArray& line)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);

    if (QJsonParseError::NoError != parseError.error || !document.isObject())
    {
        // No object means no id to echo, so the reply carries a null id and
        // the session ends: framing is unrecoverable from here.
        reject(QJsonValue(QJsonValue::Null), -32700, QStringLiteral("parse error"));
        return;
    }

    // Any complete frame proves the peer is alive, including one we reject.
    // The idle deadline slides with traffic, otherwise a client that pings on
    // schedule would still be dropped 15 s after connecting.
    m_lastSeenMs = nowMs();
    refreshIdleTimer();
    emit messageReceived(this);

    handleMessage(document.object());
}

void DataSession::handleMessage(const QJsonObject& message)
{
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QString method = message.value(QStringLiteral("method")).toString();

    // 1. Before the handshake, hello is the only legal method.
    if (!m_welcomed)
    {
        if (QStringLiteral("hello") != method)
        {
            reject(id, -32003, QStringLiteral("invalid hello"));
            return;
        }

        Hello hello;
        QString errorMessage;
        if (!parseHello(message.value(QStringLiteral("params")), hello, errorMessage))
        {
            reject(id, -32003, QStringLiteral("invalid hello: ") + errorMessage);
            return;
        }

        // Structural validity only. Capacity, duplicate ids and the protocol
        // version belong to DataServer, which answers helloReceived().
        m_hello = hello;
        m_helloId = id;
        emit helloReceived(this);
        return;
    }

    // 2. Keepalive. The server never initiates a ping, so answering is all it
    //    has to do.
    if (QStringLiteral("ping") == method)
    {
        QJsonObject result;
        result[QStringLiteral("pong")] = true;
        result[QStringLiteral("ts")] = static_cast<double>(nowMs());

        QJsonObject reply;
        reply[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        reply[QStringLiteral("id")] = id;
        reply[QStringLiteral("result")] = result;
        sendMessage(reply);
        return;
    }

    // 3. A second hello is a policy error, not a framing error: forward it so
    //    the server can answer -32003 using its own welcome state.
    if (QStringLiteral("hello") == method)
    {
        emit helloReceived(this);
        return;
    }

    // 4. Unknown method: JSON-RPC -32601 and keep the session. A newer
    //    consumer may probe for optional methods without losing the stream.
    QJsonObject error;
    error[QStringLiteral("code")] = -32601;
    error[QStringLiteral("message")] = QStringLiteral("method not found");

    QJsonObject reply;
    reply[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    reply[QStringLiteral("id")] = id;
    reply[QStringLiteral("error")] = error;
    sendMessage(reply);
}

bool DataSession::parseHello(const QJsonValue& paramsValue, Hello& hello,
                             QString& errorMessage)
{
    if (!paramsValue.isObject())
    {
        errorMessage = QStringLiteral("params must be an object");
        return false;
    }
    const QJsonObject params = paramsValue.toObject();

    // 1. protocol must be an integral JSON number. The range check exists only
    //    so the cast below cannot overflow; the version decision is made by
    //    the server (which answers -32004).
    const QJsonValue protocol = params.value(QStringLiteral("protocol"));
    if (!protocol.isDouble())
    {
        errorMessage = QStringLiteral("protocol must be an integer");
        return false;
    }
    const double protocolValue = protocol.toDouble();
    if (protocolValue != std::floor(protocolValue) || protocolValue < 0.0
        || protocolValue > 2147483647.0)
    {
        errorMessage = QStringLiteral("protocol must be a non-negative integer");
        return false;
    }

    // 2. client_id is the identity used to reject duplicates; it must be a
    //    non-empty string.
    const QJsonValue clientId = params.value(QStringLiteral("client_id"));
    if (!clientId.isString() || clientId.toString().isEmpty())
    {
        errorMessage = QStringLiteral("client_id must be a non-empty string");
        return false;
    }

    // 3. strategy is informational (it is only reported in the client list).
    //    It may be omitted, but a present value must be a string.
    const QJsonValue strategy = params.value(QStringLiteral("strategy"));
    if (!strategy.isUndefined() && !strategy.isNull() && !strategy.isString())
    {
        errorMessage = QStringLiteral("strategy must be a string");
        return false;
    }

    hello.protocol = static_cast<int>(protocolValue);
    hello.clientId = clientId.toString();
    hello.strategy = strategy.toString();
    return true;
}

void DataSession::onSocketDisconnected()
{
    emitClosedOnce();
}

void DataSession::refreshIdleTimer()
{
    m_idleTimer.start();
}

void DataSession::onIdleTimeout()
{
    closeSession();
}

void DataSession::emitClosedOnce()
{
    if (m_closedEmitted)
    {
        return;
    }
    m_closedEmitted = true;
    m_idleTimer.stop();
    m_flushTimer.stop();
    emit closed(this);
}

} // namespace pulse::client::data
