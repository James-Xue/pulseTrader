#include <QtTest>

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTcpSocket>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

#include "data/DataServer.hpp"

using pulse::client::data::DataServer;
using pulse::client::data::DataServerConfig;

namespace
{

/// Test-side consumer: one socket, its received frames, and the few helpers a
/// protocol assertion needs. It deliberately does not use QObject so it can
/// live in an anonymous namespace without a moc pass.
class TestClient
{
public:
    TestClient()
    {
        QObject::connect(&m_socket, &QTcpSocket::readyRead, &m_socket, [this]()
        {
            m_buffer.append(m_socket.readAll());

            int newline = m_buffer.indexOf('\n');
            while (newline >= 0)
            {
                const QByteArray line = m_buffer.left(newline);
                m_buffer.remove(0, newline + 1);
                if (!line.isEmpty())
                {
                    const QJsonDocument document = QJsonDocument::fromJson(line);
                    if (document.isObject())
                    {
                        m_frames.append(document.object());
                    }
                }
                newline = m_buffer.indexOf('\n');
            }
        });
    }

    bool connectTo(quint16 port)
    {
        m_socket.connectToHost(QHostAddress::LocalHost, port);
        return m_socket.waitForConnected(2000);
    }

    void send(const QJsonObject& message)
    {
        m_socket.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        m_socket.flush();
    }

    void sendHello(qint64 id, int protocol, const QString& clientId,
                   const QString& strategy)
    {
        QJsonObject params;
        params[QStringLiteral("protocol")] = protocol;
        params[QStringLiteral("client_id")] = clientId;
        params[QStringLiteral("strategy")] = strategy;

        QJsonObject message;
        message[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        message[QStringLiteral("id")] = id;
        message[QStringLiteral("method")] = QStringLiteral("hello");
        message[QStringLiteral("params")] = params;
        send(message);
    }

    void sendPing(qint64 id)
    {
        QJsonObject message;
        message[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        message[QStringLiteral("id")] = id;
        message[QStringLiteral("method")] = QStringLiteral("ping");
        send(message);
    }

    [[nodiscard]] int frameCount() const
    {
        return m_frames.size();
    }

    /// First frame with the given method, or an empty object.
    [[nodiscard]] QJsonObject frameWithMethod(const QString& method) const
    {
        for (const QJsonObject& frame : m_frames)
        {
            if (frame.value(QStringLiteral("method")).toString() == method)
            {
                return frame;
            }
        }
        return {};
    }

    /// First frame whose id matches, regardless of result or error.
    [[nodiscard]] QJsonObject frameWithId(qint64 id) const
    {
        for (const QJsonObject& frame : m_frames)
        {
            if (frame.value(QStringLiteral("id")).toInt(-1)
                == static_cast<int>(id))
            {
                return frame;
            }
        }
        return {};
    }

    [[nodiscard]] bool disconnected() const
    {
        return QAbstractSocket::UnconnectedState == m_socket.state();
    }

private:
    QTcpSocket m_socket;
    QByteArray m_buffer;
    QList<QJsonObject> m_frames;
};

/// Spin the event loop until `done()` is true or the timeout expires. The
/// server and the clients live in this thread, so blocking waits would starve
/// the server's accept and read handlers.
bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs)
    {
        QTest::qWait(10);
    }
    return done();
}

DataServerConfig testConfig()
{
    DataServerConfig config;
    config.port = 0;       // Ephemeral: never collide with a running server.
    config.pollEngine = false; // No background engine traffic in these tests.
    return config;
}

QJsonObject tickerFrame()
{
    QJsonObject ticker;
    ticker[QStringLiteral("symbol")] = QStringLiteral("XAUUSD");
    ticker[QStringLiteral("last")] = 2400.5;
    ticker[QStringLiteral("bid")] = 2400.0;
    ticker[QStringLiteral("ask")] = 2401.0;
    ticker[QStringLiteral("volume_24h")] = 1000.0;
    ticker[QStringLiteral("change_pct")] = 0.25;
    ticker[QStringLiteral("timestamp")] = 1700000000000.0;
    ticker[QStringLiteral("mark_price")] = 2400.5;
    ticker[QStringLiteral("index_price")] = 2400.5;
    ticker[QStringLiteral("funding_rate")] = 0.0001;
    return ticker;
}

QJsonArray klineFrame()
{
    QJsonObject kline;
    kline[QStringLiteral("open_time")] = 1700000000000.0;
    kline[QStringLiteral("close_time")] = 1700000059999.0;
    kline[QStringLiteral("open")] = 2400.0;
    kline[QStringLiteral("high")] = 2401.0;
    kline[QStringLiteral("low")] = 2399.0;
    kline[QStringLiteral("close")] = 2400.5;
    kline[QStringLiteral("volume")] = 12.0;
    kline[QStringLiteral("closed")] = true;

    QJsonArray array;
    array.append(kline);
    return array;
}

QJsonObject snapshotParams()
{
    QJsonObject ema;
    ema[QStringLiteral("7")] = 2400.5;
    QJsonObject indicators;
    indicators[QStringLiteral("ema")] = ema;

    QJsonObject params;
    params[QStringLiteral("engine_ready")] = true;
    params[QStringLiteral("symbol")] = QStringLiteral("XAUUSD");
    params[QStringLiteral("market_type")] = QStringLiteral("cfd");
    params[QStringLiteral("timeframe")] = QStringLiteral("1m");
    params[QStringLiteral("ticker")] = tickerFrame();
    params[QStringLiteral("klines")] = klineFrame();
    params[QStringLiteral("indicators")] = indicators;
    return params;
}

} // namespace

/// Wire-contract tests for the local data server.
///
/// Every test runs on an ephemeral port against no engine: publishSnapshot()
/// and publishUpdate() are the only inputs, which is exactly the seam the
/// production poller uses too.
class TestDataServer : public QObject
{
    Q_OBJECT

private slots:
    void welcomeThenSnapshot();
    void pingIsAnswered();
    void updateIsBroadcastWithOneSequence();
    void ninthClientIsRejectedAsFull();
    void duplicateClientIdIsRejected();
    void invalidHelloIsRejected();
    void unsupportedProtocolIsRejected();
    void clientsPropertyReportsIdentity();
    void pingKeepsSessionAlivePastIdleTimeout();
};

void TestDataServer::welcomeThenSnapshot()
{
    DataServer server(testConfig());
    QVERIFY(server.start());
    QVERIFY(server.listening());
    QVERIFY(server.port() > 0);
    QCOMPARE(server.connectedCount(), 0);

    // Seed the cache before the client connects: the welcome must be followed
    // by the cached snapshot, not by an empty placeholder.
    server.publishSnapshot(snapshotParams());
    QVERIFY(server.engineReady());

    TestClient client;
    QVERIFY(client.connectTo(static_cast<quint16>(server.port())));
    client.sendHello(1, 1, QStringLiteral("trader-01"),
                     QStringLiteral("ema-scalper"));

    QVERIFY(pumpUntil([&client]() { return client.frameCount() >= 2; }));

    // 1. The welcome is a response to id 1 and describes the dataset.
    const QJsonObject welcome = client.frameWithId(1);
    QVERIFY(!welcome.isEmpty());
    const QJsonObject result = welcome.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("protocol")).toInt(), 1);
    QCOMPARE(result.value(QStringLiteral("server")).toString(),
             QStringLiteral("pulseTrader"));
    QCOMPARE(result.value(QStringLiteral("max_clients")).toInt(), 8);
    QCOMPARE(result.value(QStringLiteral("connected")).toInt(), 1);
    QCOMPARE(result.value(QStringLiteral("engine_ready")).toBool(), true);
    QCOMPARE(result.value(QStringLiteral("symbol")).toString(),
             QStringLiteral("XAUUSD"));
    QCOMPARE(result.value(QStringLiteral("market_type")).toString(),
             QStringLiteral("cfd"));
    QCOMPARE(result.value(QStringLiteral("timeframe")).toString(),
             QStringLiteral("1m"));

    const QJsonObject indicatorParams =
        result.value(QStringLiteral("indicator_params")).toObject();
    const QJsonArray emaPeriods =
        indicatorParams.value(QStringLiteral("ema")).toArray();
    QCOMPARE(emaPeriods.size(), 5);
    QCOMPARE(emaPeriods.at(0).toInt(), 7);
    QCOMPARE(emaPeriods.at(4).toInt(), 200);
    QCOMPARE(indicatorParams.value(QStringLiteral("macd")).toObject()
                 .value(QStringLiteral("fast")).toInt(), 12);
    QCOMPARE(indicatorParams.value(QStringLiteral("supertrend")).toObject()
                 .value(QStringLiteral("multiplier")).toDouble(), 3.0);

    // 2. The snapshot follows immediately and carries the cached payload.
    const QJsonObject snapshot = client.frameWithMethod(QStringLiteral("snapshot"));
    QVERIFY(!snapshot.isEmpty());
    const QJsonObject params = snapshot.value(QStringLiteral("params")).toObject();
    QCOMPARE(params.value(QStringLiteral("seq")).toInt(), 1);
    QVERIFY(params.value(QStringLiteral("ts")).toDouble() > 0.0);
    QCOMPARE(params.value(QStringLiteral("engine_ready")).toBool(), true);
    QCOMPARE(params.value(QStringLiteral("ticker")).toObject()
                 .value(QStringLiteral("last")).toDouble(), 2400.5);
    QCOMPARE(params.value(QStringLiteral("klines")).toArray().size(), 1);

    QCOMPARE(server.connectedCount(), 1);
}

void TestDataServer::pingIsAnswered()
{
    DataServer server(testConfig());
    QVERIFY(server.start());

    TestClient client;
    QVERIFY(client.connectTo(static_cast<quint16>(server.port())));
    client.sendHello(1, 1, QStringLiteral("trader-01"), QStringLiteral("s"));
    QVERIFY(pumpUntil([&client]() { return !client.frameWithId(1).isEmpty(); }));

    client.sendPing(7);
    QVERIFY(pumpUntil([&client]() { return !client.frameWithId(7).isEmpty(); }));

    const QJsonObject reply = client.frameWithId(7);
    const QJsonObject result = reply.value(QStringLiteral("result")).toObject();
    QCOMPARE(result.value(QStringLiteral("pong")).toBool(), true);
    QVERIFY(result.value(QStringLiteral("ts")).toDouble() > 0.0);
}

void TestDataServer::updateIsBroadcastWithOneSequence()
{
    DataServer server(testConfig());
    QVERIFY(server.start());

    TestClient first;
    TestClient second;
    QVERIFY(first.connectTo(static_cast<quint16>(server.port())));
    QVERIFY(second.connectTo(static_cast<quint16>(server.port())));
    first.sendHello(1, 1, QStringLiteral("trader-01"), QStringLiteral("s1"));
    second.sendHello(1, 1, QStringLiteral("trader-02"), QStringLiteral("s2"));
    QVERIFY(pumpUntil([&server]() { return server.connectedCount() == 2; }));

    server.publishUpdate(snapshotParams());
    QVERIFY(pumpUntil([&first, &second]()
    {
        return !first.frameWithMethod(QStringLiteral("market.update")).isEmpty()
               && !second.frameWithMethod(QStringLiteral("market.update")).isEmpty();
    }));

    const QJsonObject firstParams = first.frameWithMethod(QStringLiteral("market.update"))
                                        .value(QStringLiteral("params"))
                                        .toObject();
    const QJsonObject secondParams = second.frameWithMethod(QStringLiteral("market.update"))
                                         .value(QStringLiteral("params"))
                                         .toObject();

    // One broadcast counter: both sessions must observe the same seq.
    QCOMPARE(firstParams.value(QStringLiteral("seq")).toInt(),
             secondParams.value(QStringLiteral("seq")).toInt());
    QCOMPARE(firstParams.value(QStringLiteral("engine_ready")).toBool(), true);
    QVERIFY(firstParams.value(QStringLiteral("ticker")).isObject());
}

void TestDataServer::ninthClientIsRejectedAsFull()
{
    DataServer server(testConfig());
    QVERIFY(server.start());

    // Eight consumers fill the cap; each one is welcomed before the next is
    // allowed to matter, so the ninth is deterministically the overflow.
    QList<TestClient*> clients;
    for (int i = 0; i < 8; ++i)
    {
        auto* client = new TestClient();
        clients.append(client);
    }
    for (int i = 0; i < clients.size(); ++i)
    {
        QVERIFY(clients.at(i)->connectTo(static_cast<quint16>(server.port())));
        clients.at(i)->sendHello(
            1, 1, QStringLiteral("trader-%1").arg(i), QStringLiteral("s"));
    }
    QVERIFY(pumpUntil([&server]() { return server.connectedCount() == 8; }));

    TestClient overflow;
    QVERIFY(overflow.connectTo(static_cast<quint16>(server.port())));
    overflow.sendHello(1, 1, QStringLiteral("trader-09"), QStringLiteral("s"));
    QVERIFY(pumpUntil([&overflow]() { return !overflow.frameWithId(1).isEmpty(); }));

    const QJsonObject error =
        overflow.frameWithId(1).value(QStringLiteral("error")).toObject();
    QCOMPARE(error.value(QStringLiteral("code")).toInt(), -32002);
    QCOMPARE(server.connectedCount(), 8);

    // The rejection is terminal: the server closes the socket right after it.
    QVERIFY(pumpUntil([&overflow]() { return overflow.disconnected(); }));

    qDeleteAll(clients);
}

void TestDataServer::duplicateClientIdIsRejected()
{
    DataServer server(testConfig());
    QVERIFY(server.start());

    TestClient first;
    QVERIFY(first.connectTo(static_cast<quint16>(server.port())));
    first.sendHello(1, 1, QStringLiteral("trader-01"), QStringLiteral("s"));
    QVERIFY(pumpUntil([&first]() { return !first.frameWithId(1).isEmpty(); }));

    TestClient duplicate;
    QVERIFY(duplicate.connectTo(static_cast<quint16>(server.port())));
    duplicate.sendHello(2, 1, QStringLiteral("trader-01"), QStringLiteral("s"));
    QVERIFY(pumpUntil([&duplicate]()
    {
        return !duplicate.frameWithId(2).isEmpty();
    }));

    const QJsonObject error =
        duplicate.frameWithId(2).value(QStringLiteral("error")).toObject();
    QCOMPARE(error.value(QStringLiteral("code")).toInt(), -32001);
    QCOMPARE(server.connectedCount(), 1);
    QVERIFY(pumpUntil([&duplicate]() { return duplicate.disconnected(); }));
}

void TestDataServer::invalidHelloIsRejected()
{
    DataServer server(testConfig());
    QVERIFY(server.start());

    TestClient client;
    QVERIFY(client.connectTo(static_cast<quint16>(server.port())));

    // client_id is mandatory; a request without it is structurally invalid.
    QJsonObject params;
    params[QStringLiteral("protocol")] = 1;
    params[QStringLiteral("strategy")] = QStringLiteral("s");

    QJsonObject message;
    message[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    message[QStringLiteral("id")] = 5;
    message[QStringLiteral("method")] = QStringLiteral("hello");
    message[QStringLiteral("params")] = params;
    client.send(message);

    QVERIFY(pumpUntil([&client]() { return !client.frameWithId(5).isEmpty(); }));
    const QJsonObject error =
        client.frameWithId(5).value(QStringLiteral("error")).toObject();
    QCOMPARE(error.value(QStringLiteral("code")).toInt(), -32003);
    QCOMPARE(server.connectedCount(), 0);
}

void TestDataServer::unsupportedProtocolIsRejected()
{
    DataServer server(testConfig());
    QVERIFY(server.start());

    TestClient client;
    QVERIFY(client.connectTo(static_cast<quint16>(server.port())));
    client.sendHello(3, 2, QStringLiteral("trader-01"), QStringLiteral("s"));

    QVERIFY(pumpUntil([&client]() { return !client.frameWithId(3).isEmpty(); }));
    const QJsonObject error =
        client.frameWithId(3).value(QStringLiteral("error")).toObject();
    QCOMPARE(error.value(QStringLiteral("code")).toInt(), -32004);
    QCOMPARE(server.connectedCount(), 0);
}

void TestDataServer::clientsPropertyReportsIdentity()
{
    DataServer server(testConfig());
    QVERIFY(server.start());

    TestClient client;
    QVERIFY(client.connectTo(static_cast<quint16>(server.port())));
    client.sendHello(1, 1, QStringLiteral("trader-01"),
                     QStringLiteral("ema-scalper"));
    QVERIFY(pumpUntil([&server]() { return server.connectedCount() == 1; }));

    const QVariantList clients = server.clients();
    QCOMPARE(clients.size(), 1);

    const QVariantMap entry = clients.first().toMap();
    QCOMPARE(entry.value(QStringLiteral("client_id")).toString(),
             QStringLiteral("trader-01"));
    QCOMPARE(entry.value(QStringLiteral("strategy")).toString(),
             QStringLiteral("ema-scalper"));
    QVERIFY(entry.value(QStringLiteral("connected_at")).toLongLong() > 0);
    QVERIFY(entry.value(QStringLiteral("last_seen_ms")).toLongLong() > 0);
}

void TestDataServer::pingKeepsSessionAlivePastIdleTimeout()
{
    // A short timeout keeps the test fast; the production value is 15 s.
    DataServerConfig config = testConfig();
    config.clientIdleTimeoutMs = 600;

    DataServer server(config);
    QVERIFY(server.start());

    TestClient client;
    QVERIFY(client.connectTo(static_cast<quint16>(server.port())));
    client.sendHello(1, 1, QStringLiteral("trader-01"), QStringLiteral("s"));
    QVERIFY(pumpUntil([&client]() { return !client.frameWithId(1).isEmpty(); }));

    // Three timeout windows of steady pings must not end the session.
    for (int i = 0; i < 12; ++i)
    {
        QTest::qWait(150);
        client.sendPing(100 + i);
    }
    QVERIFY(!client.disconnected());
    QCOMPARE(server.connectedCount(), 1);

    // Going silent is what the watchdog is for.
    QVERIFY(pumpUntil([&client]() { return client.disconnected(); }, 3000));
    QVERIFY(pumpUntil([&server]() { return server.connectedCount() == 0; }, 2000));
}

QTEST_MAIN(TestDataServer)
#include "tst_dataserver.moc"
