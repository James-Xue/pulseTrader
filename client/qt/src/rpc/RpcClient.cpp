#include "rpc/RpcClient.hpp"

#include <QJsonDocument>
#include <QJsonValue>
#include <QQueue>
#include <QSet>
#include <QSslSocket>
#include <QTcpSocket>
#include <QtNetwork>

#include <algorithm>

namespace pulse::client
{

namespace
{
/// Bounded memory of recently expired request ids. A reply that arrives after
/// its timeout is discarded instead of being reported as a protocol desync.
constexpr int kMaxExpiredIds = 64;
} // namespace

RpcClient::RpcClient(QString name, QObject* parent)
    : QObject(parent)
    , m_name(std::move(name))
{
    qRegisterMetaType<RpcError>("pulse::client::RpcError");

    // A single-shot timer owns the retry cadence; every path that wants to
    // reconnect arms it instead of calling connectToEngine() directly, so the
    // backoff cannot be bypassed by accident.
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]()
    {
        if (m_wantConnected)
        {
            connectToEngine();
        }
    });
}

RpcClient::~RpcClient()
{
    m_wantConnected = false;
    m_reconnectTimer.stop();
    failAll(RpcError{ RpcErrorKind::Transport, QStringLiteral("client destroyed"), 0, 0 });
    if (nullptr != m_socket)
    {
        m_socket->abort();
    }
}

void RpcClient::setEndpoint(const QString& host, quint16 port, bool tls)
{
    // 1. Ignore a no-op change so a settings page that re-applies the same
    //    values does not tear down a healthy connection.
    // 2. Otherwise store the new endpoint and force a reconnect when the
    //    caller already asked for a live connection.
    if (host == m_host && port == m_port && tls == m_tls)
    {
        return;
    }
    m_host = host;
    m_port = port;
    m_tls = tls;

    if (m_wantConnected)
    {
        m_reconnectTimer.stop();
        if (nullptr != m_socket)
        {
            m_socket->abort();
        }
        connectToEngine();
    }
}

void RpcClient::setToken(const QString& token)
{
    m_token = token;
}

void RpcClient::connectToEngine()
{
    m_wantConnected = true;

    // 1. Drop the previous socket. Recreating it keeps signal wiring and TLS
    //    state from leaking across attempts.
    if (nullptr != m_socket)
    {
        m_socket->disconnect(this);
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    failAll(RpcError{ RpcErrorKind::Transport, QStringLiteral("reconnecting"), 0, 0 });
    m_buffer.clear();

    // 2. Pick the transport. The TLS branch is wired now so M-Q4 only has to
    //    add the auth exchange, not restructure the connection.
    if (m_tls)
    {
        auto* ssl = new QSslSocket(this);
        connect(ssl, &QSslSocket::encrypted, this, &RpcClient::onConnected);
        connect(ssl, &QSslSocket::sslErrors, this, [this](const QList<QSslError>& errors)
        {
            QStringList text;
            for (const QSslError& error : errors)
            {
                text << error.errorString();
            }
            failAll(RpcError{ RpcErrorKind::TlsHandshake, text.join(QStringLiteral("; ")), 0, 0 });
            m_socket->abort();
        });
        m_socket = ssl;
    }
    else
    {
        m_socket = new QTcpSocket(this);
        connect(m_socket, &QTcpSocket::connected, this, &RpcClient::onConnected);
    }

    connect(m_socket, &QTcpSocket::readyRead, this, &RpcClient::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &RpcClient::onDisconnected);

    setState(State::Connecting);
    if (m_tls)
    {
        static_cast<QSslSocket*>(m_socket)->connectToHostEncrypted(m_host, m_port);
    }
    else
    {
        m_socket->connectToHost(m_host, m_port);
    }
}

void RpcClient::disconnectFromEngine()
{
    m_wantConnected = false;
    m_reconnectTimer.stop();
    failAll(RpcError{ RpcErrorKind::Transport, QStringLiteral("disconnected by user"), 0, 0 });
    if (nullptr != m_socket)
    {
        m_socket->abort();
    }
    m_buffer.clear();
    setState(State::Disconnected);
}

qint64 RpcClient::call(const QString& method, const QJsonObject& params, int timeoutMs)
{
    const qint64 id = m_nextId++;

    // 1. Refuse early when the socket is not ready. The failure is delivered
    //    asynchronously so the caller can finish connecting its own slots.
    if (State::Ready != m_state || nullptr == m_socket)
    {
        const RpcError error{ RpcErrorKind::Transport,
                              QStringLiteral("not connected"), 0, 0 };
        QMetaObject::invokeMethod(this, [this, id, error]()
        {
            emit callFailed(id, error);
        }, Qt::QueuedConnection);
        return id;
    }

    // 2. Build the envelope. The engine requires jsonrpc, id and method; params
    //    is passed as an object because every handler ignores it except
    //    get_market, which reads named fields.
    QJsonObject request;
    request[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    request[QStringLiteral("id")] = id;
    request[QStringLiteral("method")] = method;
    if (!params.isEmpty())
    {
        request[QStringLiteral("params")] = params;
    }

    // 3. Arm the per-call timer before writing, so a reply that arrives during
    //    the write is still correlated.
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id]()
    {
        clearPending(id);
        emit callFailed(id, RpcError{ RpcErrorKind::Timeout,
                                      QStringLiteral("no reply"), 0, 0 });
    });
    timer->start(timeoutMs);
    m_pending.insert(id, Pending{ method, timer });

    // 4. One compact JSON object per line. The engine splits on '\n' and never
    //    reads a length prefix.
    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    line.append('\n');
    const qint64 written = m_socket->write(line);
    if (written != line.size())
    {
        clearPending(id);
        emit callFailed(id, RpcError{ RpcErrorKind::Transport,
                                      QStringLiteral("short write"), 0, 0 });
    }
    return id;
}

void RpcClient::setState(State state)
{
    if (state == m_state)
    {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

void RpcClient::failAll(const RpcError& error)
{
    // Copy the keys first: clearPending() mutates the hash.
    const QList<qint64> ids = m_pending.keys();
    for (qint64 id : ids)
    {
        clearPending(id);
        emit callFailed(id, error);
    }
}

void RpcClient::clearPending(qint64 id)
{
    auto it = m_pending.find(id);
    if (it == m_pending.end())
    {
        return;
    }
    if (nullptr != it->timer)
    {
        it->timer->stop();
        it->timer->deleteLater();
    }
    m_pending.erase(it);
}

void RpcClient::onConnected()
{
    m_backoffMs = 1000;

    // 1. No token configured: go straight to Ready. If the engine actually
    //    requires one, the first real request returns 9106 and the call fails
    //    with AuthFailed, which the UI renders as "authentication required".
    if (m_token.isEmpty())
    {
        setState(State::Ready);
        return;
    }

    // 2. Token configured: negotiate before any other request. The engine
    //    accepts exactly one auth line and nothing else first.
    setState(State::Authenticating);

    const qint64 id = m_nextId++;
    m_authId = id;

    QJsonObject params;
    params[QStringLiteral("token")] = m_token;

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id]()
    {
        clearPending(id);
        m_wantConnected = false;
        emit authenticationFailed(QStringLiteral("auth handshake timed out"));
        if (nullptr != m_socket)
        {
            m_socket->abort();
        }
    });
    timer->start(5000);
    m_pending.insert(id, Pending{ QStringLiteral("auth"), timer });

    QJsonObject request;
    request[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    request[QStringLiteral("id")] = id;
    request[QStringLiteral("method")] = QStringLiteral("auth");
    request[QStringLiteral("params")] = params;

    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    line.append('\n');
    m_socket->write(line);
}

void RpcClient::onDisconnected()
{
    m_authId = -1;
    failAll(RpcError{ RpcErrorKind::Transport, QStringLiteral("connection closed"), 0, 0 });
    m_buffer.clear();
    setState(State::Disconnected);

    if (m_wantConnected)
    {
        scheduleReconnect();
    }
}

void RpcClient::scheduleReconnect()
{
    m_reconnectTimer.start(m_backoffMs);
    m_backoffMs = std::min(m_backoffMs * 2, kMaxBackoffMs);
}

void RpcClient::onReadyRead()
{
    m_buffer.append(m_socket->readAll());

    // 1. Split on '\n'. The engine terminates every reply with exactly one.
    int newline = m_buffer.indexOf('\n');
    while (newline >= 0)
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

    // 2. A peer that streams without a delimiter is either broken or hostile.
    //    Drop the connection rather than growing the buffer without bound.
    if (m_buffer.size() > kMaxFrameBytes)
    {
        failAll(RpcError{ RpcErrorKind::ProtocolDesync,
                          QStringLiteral("frame exceeds 1 MiB"), 0, 0 });
        m_buffer.clear();
        m_socket->abort();
    }
}

void RpcClient::handleLine(const QByteArray& line)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);

    // 1. Non-JSON or non-object payloads are a protocol desync: the engine only
    //    ever writes JSON objects.
    if (QJsonParseError::NoError != parseError.error || !document.isObject())
    {
        failAll(RpcError{ RpcErrorKind::ProtocolDesync,
                          QStringLiteral("unparseable reply"), 0, 0 });
        m_socket->abort();
        return;
    }

    handleReply(document.object());
}

void RpcClient::handleReply(const QJsonObject& reply)
{
    // 1. A reply without an id cannot be correlated. The engine never sends
    //    notifications, so this is a desync.
    if (!reply.contains(QStringLiteral("id")))
    {
        failAll(RpcError{ RpcErrorKind::ProtocolDesync,
                          QStringLiteral("reply without id"), 0, 0 });
        m_socket->abort();
        return;
    }

    const QJsonValue idValue = reply.value(QStringLiteral("id"));
    if (!idValue.isDouble())
    {
        failAll(RpcError{ RpcErrorKind::ProtocolDesync,
                          QStringLiteral("non-numeric id"), 0, 0 });
        m_socket->abort();
        return;
    }

    const qint64 id = static_cast<qint64>(idValue.toDouble());

    // 2. A late reply for an already-timed-out call is dropped silently.
    auto pendingIt = m_pending.find(id);
    if (pendingIt == m_pending.end())
    {
        return;
    }

    // Capture the method before clearPending invalidates the iterator: the
    // auth reply is routed differently from a normal call reply.
    const QString method = pendingIt->method;
    clearPending(id);

    const bool hasError = reply.contains(QStringLiteral("error"));

    if (QStringLiteral("auth") == method)
    {
        if (hasError)
        {
            const QJsonObject error = reply.value(QStringLiteral("error")).toObject();
            // A rejected token is not transient: stop reconnecting so the UI
            // can show the reason instead of a reconnect loop.
            m_wantConnected = false;
            m_authId = -1;
            emit authenticationFailed(
                error.value(QStringLiteral("message")).toString());
            setState(State::Disconnected);
            m_socket->abort();
            return;
        }
        m_authId = -1;
        setState(State::Ready);
        return;
    }

    // 3. Error object shape: code, message, and data.code carrying the
    //    PulseError value. Both codes are preserved.
    if (hasError)
    {
        const QJsonObject error = reply.value(QStringLiteral("error")).toObject();
        RpcError rpcError;
        rpcError.kind = RpcErrorKind::RpcError;
        rpcError.message = error.value(QStringLiteral("message")).toString();
        rpcError.jsonRpcCode = error.value(QStringLiteral("code")).toInt();
        rpcError.dataCode = error.value(QStringLiteral("data")).toObject()
                                 .value(QStringLiteral("code")).toInt();
        emit callFailed(id, rpcError);
        return;
    }

    // 4. Success: every read-only method returns a JSON object.
    const QJsonObject result = reply.value(QStringLiteral("result")).toObject();
    emit callSucceeded(id, result);
}

} // namespace pulse::client
