#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include "rpc/RpcError.hpp"

class QTcpSocket;

namespace pulse::client
{

/// One control-socket connection speaking newline-delimited JSON-RPC 2.0.
///
/// Thread affinity: lives on the GUI thread and never blocks. Every socket
/// operation is asynchronous; call() only writes and arms a timer.
///
/// Protocol facts this class depends on (verified against src/control/):
/// 1. The server writes exactly one compact JSON object followed by '\n'.
/// 2. It echoes the request id verbatim, including a non-numeric id.
/// 3. It never pushes unsolicited messages, so an unknown id is a desync.
/// 4. The server has no frame limit, so the client enforces its own.
class RpcClient : public QObject
{
    Q_OBJECT

public:
    /// Connection lifecycle as seen by the UI.
    enum class State
    {
        Disconnected,   ///< No socket, or a reconnect is pending.
        Connecting,     ///< TCP connect (and TLS handshake) in progress.
        Authenticating, ///< Token handshake sent, waiting for the verdict.
        Ready,          ///< Connected and able to carry calls.
    };
    Q_ENUM(State)

    explicit RpcClient(QString name, QObject* parent = nullptr);
    ~RpcClient() override;

    /// Configure the endpoint. A state change while connected forces a reconnect.
    void setEndpoint(const QString& host, quint16 port, bool tls);

    /// Store the bearer token. The auth handshake itself lands in M-Q4; until
    /// then the token is unused and never written to the socket.
    void setToken(const QString& token);

    /// Offset the request-id space. Each connection numbers its own requests
    /// from 1, so a caller that routes replies through one shared table must
    /// give every RpcClient a disjoint range or the ids collide.
    void setIdBase(qint64 base);

    /// Start connecting and keep reconnecting until disconnectFromEngine().
    void connectToEngine();

    /// Stop reconnecting and close the socket now.
    void disconnectFromEngine();

    /// Send one request. Returns the request id; the reply arrives through
    /// callSucceeded() or callFailed(). The timeout is per call, in ms.
    [[nodiscard]] qint64 call(const QString& method, const QJsonObject& params,
                              int timeoutMs);

    [[nodiscard]] State state() const
    {
        return m_state;
    }

    [[nodiscard]] QString name() const
    {
        return m_name;
    }

signals:
    void stateChanged(pulse::client::RpcClient::State state);
    void callSucceeded(qint64 id, const QJsonObject& result);
    void callFailed(qint64 id, const pulse::client::RpcError& error);

    /// The engine rejected the configured token. Retrying is pointless, so the
    /// client stops reconnecting until connectToEngine() is called again.
    void authenticationFailed(const QString& message);

private:
    /// Per-request bookkeeping: the method name for diagnostics and the timer
    /// that turns a missing reply into a Timeout error.
    struct Pending
    {
        QString method;
        QTimer* timer = nullptr;
    };

    static constexpr int kMaxFrameBytes = 1 << 20; ///< 1 MiB per line.
    static constexpr int kMaxBackoffMs = 30000;

    void setState(State state);
    void failAll(const RpcError& error);
    void onReadyRead();
    void onConnected();
    void onDisconnected();
    void handleLine(const QByteArray& line);
    void handleReply(const QJsonObject& reply);
    void scheduleReconnect();
    void clearPending(qint64 id);

    QString m_name;
    QString m_host = QStringLiteral("127.0.0.1");
    quint16 m_port = 8081;
    bool m_tls = false;
    QString m_token;

    QTcpSocket* m_socket = nullptr;
    QByteArray m_buffer;
    QHash<qint64, Pending> m_pending;
    qint64 m_nextId = 1;
    qint64 m_authId = -1; ///< Request id of the in-flight auth handshake.

    State m_state = State::Disconnected;
    bool m_wantConnected = false;
    int m_backoffMs = 1000;
    QTimer m_reconnectTimer;
};

} // namespace pulse::client

Q_DECLARE_METATYPE(pulse::client::RpcError)
