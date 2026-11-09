#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QTimer>

#include "data/DataServerConfig.hpp"

class QTcpSocket;

namespace pulse::client::data
{

/// One consumer connection speaking newline-delimited JSON.
///
/// The session owns framing and the JSON-RPC envelope mechanics; policy
/// (capacity, duplicate ids, protocol version) lives in DataServer, which
/// answers the helloReceived() signal. That split keeps the transport testable
/// on its own and gives the server one place to see every connected consumer.
///
/// Thread affinity: GUI thread, like every other socket wrapper in the client.
class DataSession : public QObject
{
    Q_OBJECT

public:
    /// Identity a consumer presents in its hello.
    struct Hello
    {
        int protocol = 0;
        QString clientId;
        QString strategy;
    };

    DataSession(QTcpSocket* socket, const DataServerConfig& config,
                QObject* parent = nullptr);
    ~DataSession() override;

    /// Write one complete JSON object followed by '\n'. Silently dropped when
    /// the socket is already gone; a write that cannot drain is closed instead
    /// of buffered without bound.
    void sendMessage(const QJsonObject& message);

    /// Send a JSON-RPC error response and close the session. Used for the four
    /// protocol rejection codes, which are terminal by definition.
    void reject(const QJsonValue& id, int code, const QString& message);

    /// Flush pending bytes and close. Safe to call more than once.
    void closeSession();

    /// Record the identity the server accepted. connected_at starts counting
    /// here, not at TCP accept: a socket that never said hello was never a
    /// client of the dataset.
    void acceptHello(const Hello& hello);

    [[nodiscard]] bool welcomed() const
    {
        return m_welcomed;
    }

    [[nodiscard]] const Hello& hello() const
    {
        return m_hello;
    }

    [[nodiscard]] QJsonValue helloId() const
    {
        return m_helloId;
    }

    [[nodiscard]] QString clientId() const
    {
        return m_hello.clientId;
    }

    [[nodiscard]] QString strategy() const
    {
        return m_hello.strategy;
    }

    [[nodiscard]] qint64 connectedAtMs() const
    {
        return m_connectedAtMs;
    }

    [[nodiscard]] qint64 lastSeenMs() const
    {
        return m_lastSeenMs;
    }

signals:
    /// A structurally valid hello arrived. The server must answer with either
    /// acceptHello() + a welcome, or reject().
    void helloReceived(pulse::client::data::DataSession* session);

    /// Some complete frame arrived; last_seen was refreshed.
    void messageReceived(pulse::client::data::DataSession* session);

    /// The socket is gone, or closeSession() finished. Emitted exactly once.
    void closed(pulse::client::data::DataSession* session);

private:
    /// Append incoming bytes, split on '\n', and dispatch every complete line.
    void onReadyRead();

    /// Parse one framed line and hand the object to handleMessage().
    void handleLine(const QByteArray& line);

    /// Handle one parsed frame: hello, ping or an unknown method.
    void handleMessage(const QJsonObject& message);

    void onSocketDisconnected();
    void onIdleTimeout();
    void emitClosedOnce();
    void refreshIdleTimer();

    static bool parseHello(const QJsonValue& paramsValue, Hello& hello,
                           QString& errorMessage);

    QTcpSocket* m_socket = nullptr;
    DataServerConfig m_config;
    QByteArray m_buffer;

    /// Framing limit. The client of the engine enforces 1 MiB per line for the
    /// same reason: a peer that streams without a delimiter must not grow the
    /// buffer until the process dies.
    static constexpr int kMaxFrameBytes = 1 << 20;

    /// Grace period between "asked to close" and "aborted". A peer that never
    /// reads would otherwise keep the socket (and the session) alive forever.
    static constexpr int kFlushGraceMs = 2000;

    QTimer m_idleTimer;
    QTimer m_flushTimer;

    Hello m_hello;
    QJsonValue m_helloId;
    bool m_welcomed = false;
    bool m_closing = false;
    bool m_closedEmitted = false;
    qint64 m_connectedAtMs = 0;
    qint64 m_lastSeenMs = 0;
};

} // namespace pulse::client::data
