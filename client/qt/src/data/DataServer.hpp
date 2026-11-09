#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include "data/DataServerConfig.hpp"

class QTcpServer;

namespace pulse::client::data
{

class DataSession;
class MarketDataService;

/// Local market-data server for downstream consumers (the gold-trader bot).
///
/// It exposes a loopback-only, newline-delimited JSON-RPC endpoint that pushes
/// one shared dataset to every connected client:
/// 1. A client says hello with a protocol version, a client_id and a strategy
///    name; the server answers with a welcome carrying the dataset shape.
/// 2. The welcome is followed immediately by a full snapshot (ticker, 500
///    candles, indicators) so a late joiner does not wait for its baseline.
/// 3. Every engine poll produces one light "market.update" broadcast, at most
///    2 Hz, carrying the ticker, the newest candle and the indicators.
///
/// The engine side is a MarketDataService instance that owns its own control
/// socket; publishSnapshot()/publishUpdate() are public so the network layer
/// can be exercised without an engine.
///
/// Thread affinity: GUI thread. Sockets are asynchronous, so nothing here can
/// block the UI while the engine is slow or absent.
class DataServer : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(int port READ port NOTIFY listeningChanged)
    Q_PROPERTY(bool engineReady READ engineReady NOTIFY engineReadyChanged)
    Q_PROPERTY(int connectedCount READ connectedCount NOTIFY connectedCountChanged)
    Q_PROPERTY(int maxClients READ maxClients CONSTANT)
    Q_PROPERTY(QVariantList clients READ clients NOTIFY clientsChanged)

public:
    explicit DataServer(QObject* parent = nullptr);
    explicit DataServer(const DataServerConfig& config, QObject* parent = nullptr);
    ~DataServer() override;

    /// Bind the listener and (with pollEngine) start the engine poller.
    /// Returns false when the port cannot be bound; the reason is emitted
    /// through errorOccurred() so QML can surface it without a dialog here.
    Q_INVOKABLE bool start();

    /// Stop the poller and close every session and the listener.
    Q_INVOKABLE void stop();

    /// Broadcast one full snapshot and cache it for future hellos.
    void publishSnapshot(const QJsonObject& params);

    /// Broadcast one light update. Not cached: a client that missed it gets
    /// the next one within one poll interval.
    void publishUpdate(const QJsonObject& params);

    [[nodiscard]] bool listening() const;
    [[nodiscard]] int port() const;

    /// True once the engine poller has produced usable data. Also updated from
    /// the engine_ready field of every published payload, so a direct publish
    /// (tests, tools) keeps the property honest.
    [[nodiscard]] bool engineReady() const
    {
        return m_engineReady;
    }

    [[nodiscard]] int connectedCount() const
    {
        return m_connectedCount;
    }

    [[nodiscard]] int maxClients() const
    {
        return m_config.maxClients;
    }

    /// Welcomed sessions as maps of client_id, strategy, connected_at and
    /// last_seen_ms. Pending sockets are not clients yet and stay hidden.
    [[nodiscard]] QVariantList clients() const;

    [[nodiscard]] const DataServerConfig& config() const
    {
        return m_config;
    }

signals:
    void listeningChanged();
    void engineReadyChanged();
    void connectedCountChanged();
    void clientsChanged();
    void errorOccurred(const QString& message);

private:
    void onNewConnection();
    void onHello(DataSession* session);
    void onSessionMessage(DataSession* session);
    void onSessionClosed(DataSession* session);

    /// The welcome result object: dataset identity plus indicator periods.
    [[nodiscard]] QJsonObject welcome() const;

    /// Cached snapshot, or a well-formed empty one before the first poll.
    [[nodiscard]] QJsonObject currentSnapshot() const;

    /// Copy `params`, assign the next broadcast sequence number, and give it a
    /// timestamp when the producer left one out.
    [[nodiscard]] QJsonObject stamp(const QJsonObject& params);

    void broadcast(const QString& method, const QJsonObject& params);
    void applyEngineReady(const QJsonObject& params);
    void setEngineReady(bool ready);

    DataServerConfig m_config;
    QTcpServer* m_tcpServer = nullptr;
    MarketDataService* m_market = nullptr;
    QList<DataSession*> m_sessions;

    /// Single broadcast counter. Every client sees the same value for a given
    /// frame, and only this counter advances it.
    qint64 m_seq = 0;

    int m_connectedCount = 0;
    bool m_engineReady = false;
    QJsonObject m_cachedSnapshot;
};

} // namespace pulse::client::data
