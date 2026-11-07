#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "rpc/RpcClient.hpp"

using pulse::client::RpcClient;
using pulse::client::RpcError;
using pulse::client::RpcErrorKind;

/// Transport contract tests.
///
/// These exercise the client against a scripted local server, so they run with
/// no engine and no network. Each test owns its server and disconnects it from
/// the test object before returning: a lambda that outlives its captures would
/// be a use-after-scope.
class TestRpcClient : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip();
    void errorMapping();
    void timeoutProducesTimeoutError();
    void splitFramesAreReassembled();
    void unknownReplyIdIsIgnored();
    void reconnectsAfterInitialRefusal();

private:
    /// Read one newline-terminated request from `peer` and return it parsed.
    static QJsonObject takeRequest(QByteArray& buffer)
    {
        const int newline = buffer.indexOf('\n');
        if (newline < 0)
        {
            return {};
        }
        const QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        return QJsonDocument::fromJson(line).object();
    }

    static void writeReply(QTcpSocket* peer, qint64 id, const QJsonObject& payload)
    {
        QJsonObject reply;
        reply[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        reply[QStringLiteral("id")] = id;
        if (payload.contains(QStringLiteral("error")))
        {
            reply[QStringLiteral("error")] = payload.value(QStringLiteral("error"));
        }
        else
        {
            reply[QStringLiteral("result")] = payload.value(QStringLiteral("result"));
        }
        peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
        peer->flush();
    }
};

void TestRpcClient::roundTrip()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    RpcClient client(QStringLiteral("test"));
    QSignalSpy ok(&client, &RpcClient::callSucceeded);
    QSignalSpy failed(&client, &RpcClient::callFailed);

    QObject context;
    QByteArray buffer;
    connect(&server, &QTcpServer::newConnection, &context, [&server, &buffer, &context]()
    {
        QTcpSocket* peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, &context, [peer, &buffer]()
        {
            buffer.append(peer->readAll());
            const QJsonObject request = TestRpcClient::takeRequest(buffer);
            if (request.isEmpty())
            {
                return;
            }
            QJsonObject result;
            result[QStringLiteral("version")] = QStringLiteral("0.1.0");
            QJsonObject payload;
            payload[QStringLiteral("result")] = result;
            TestRpcClient::writeReply(
                peer, static_cast<qint64>(request.value(QStringLiteral("id")).toDouble()),
                payload);
        });
    });

    client.setEndpoint(QStringLiteral("127.0.0.1"), server.serverPort(), false);
    client.connectToEngine();
    QTRY_VERIFY_WITH_TIMEOUT(RpcClient::State::Ready == client.state(), 3000);

    client.call(QStringLiteral("get_status"), QJsonObject{}, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(1 == ok.count(), 3000);
    QCOMPARE(0, failed.count());

    const QList<QVariant> args = ok.takeFirst();
    const QJsonObject result = args.at(1).toJsonObject();
    QCOMPARE(QStringLiteral("0.1.0"), result.value(QStringLiteral("version")).toString());

    disconnect(&server, nullptr, &context, nullptr);
}

void TestRpcClient::errorMapping()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    RpcClient client(QStringLiteral("test"));
    QSignalSpy failed(&client, &RpcClient::callFailed);

    QObject context;
    QByteArray buffer;
    connect(&server, &QTcpServer::newConnection, &context, [&server, &buffer, &context]()
    {
        QTcpSocket* peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, &context, [peer, &buffer]()
        {
            buffer.append(peer->readAll());
            const QJsonObject request = TestRpcClient::takeRequest(buffer);
            if (request.isEmpty())
            {
                return;
            }
            QJsonObject data;
            data[QStringLiteral("code")] = 9103;
            QJsonObject error;
            error[QStringLiteral("code")] = -32603;
            error[QStringLiteral("message")] = QStringLiteral("handler threw");
            error[QStringLiteral("data")] = data;
            QJsonObject payload;
            payload[QStringLiteral("error")] = error;
            TestRpcClient::writeReply(
                peer, static_cast<qint64>(request.value(QStringLiteral("id")).toDouble()),
                payload);
        });
    });

    client.setEndpoint(QStringLiteral("127.0.0.1"), server.serverPort(), false);
    client.connectToEngine();
    QTRY_VERIFY_WITH_TIMEOUT(RpcClient::State::Ready == client.state(), 3000);

    client.call(QStringLiteral("get_status"), QJsonObject{}, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(1 == failed.count(), 3000);

    const QList<QVariant> args = failed.takeFirst();
    const auto error = args.at(1).value<RpcError>();
    QCOMPARE(static_cast<int>(RpcErrorKind::RpcError), static_cast<int>(error.kind));
    QCOMPARE(-32603, error.jsonRpcCode);
    QCOMPARE(9103, error.dataCode);

    disconnect(&server, nullptr, &context, nullptr);
}

void TestRpcClient::timeoutProducesTimeoutError()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    RpcClient client(QStringLiteral("test"));
    QSignalSpy failed(&client, &RpcClient::callFailed);

    QObject context;
    connect(&server, &QTcpServer::newConnection, &context, [&server, &context]()
    {
        // Accept and read, but never answer: the client must time out.
        QTcpSocket* peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, &context, [peer]() { peer->readAll(); });
    });

    client.setEndpoint(QStringLiteral("127.0.0.1"), server.serverPort(), false);
    client.connectToEngine();
    QTRY_VERIFY_WITH_TIMEOUT(RpcClient::State::Ready == client.state(), 3000);

    client.call(QStringLiteral("get_status"), QJsonObject{}, 250);
    QTRY_VERIFY_WITH_TIMEOUT(1 == failed.count(), 3000);

    const auto error = failed.takeFirst().at(1).value<RpcError>();
    QCOMPARE(static_cast<int>(RpcErrorKind::Timeout), static_cast<int>(error.kind));

    disconnect(&server, nullptr, &context, nullptr);
}

void TestRpcClient::splitFramesAreReassembled()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    RpcClient client(QStringLiteral("test"));
    QSignalSpy ok(&client, &RpcClient::callSucceeded);

    QObject context;
    QByteArray buffer;
    connect(&server, &QTcpServer::newConnection, &context, [&server, &buffer, &context]()
    {
        QTcpSocket* peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, &context, [peer, &buffer]()
        {
            buffer.append(peer->readAll());
            const QJsonObject request = TestRpcClient::takeRequest(buffer);
            if (request.isEmpty())
            {
                return;
            }
            QJsonObject reply;
            reply[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
            reply[QStringLiteral("id")] =
                static_cast<qint64>(request.value(QStringLiteral("id")).toDouble());
            QJsonObject result;
            result[QStringLiteral("split")] = true;
            reply[QStringLiteral("result")] = result;

            const QByteArray line = QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n';
            const int half = line.size() / 2;
            peer->write(line.left(half));
            peer->flush();
            // Second half arrives on a later event-loop turn.
            QTimer::singleShot(50, peer, [peer, line, half]()
            {
                peer->write(line.mid(half));
                peer->flush();
            });
        });
    });

    client.setEndpoint(QStringLiteral("127.0.0.1"), server.serverPort(), false);
    client.connectToEngine();
    QTRY_VERIFY_WITH_TIMEOUT(RpcClient::State::Ready == client.state(), 3000);

    client.call(QStringLiteral("get_status"), QJsonObject{}, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(1 == ok.count(), 3000);
    QCOMPARE(true, ok.takeFirst().at(1).toJsonObject()
                       .value(QStringLiteral("split")).toBool());

    disconnect(&server, nullptr, &context, nullptr);
}

void TestRpcClient::unknownReplyIdIsIgnored()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    RpcClient client(QStringLiteral("test"));
    QSignalSpy ok(&client, &RpcClient::callSucceeded);
    QSignalSpy failed(&client, &RpcClient::callFailed);

    QObject context;
    connect(&server, &QTcpServer::newConnection, &context, [&server, &context]()
    {
        QTcpSocket* peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, &context, [peer]()
        {
            peer->readAll();
            QJsonObject result;
            result[QStringLiteral("stale")] = true;
            QJsonObject payload;
            payload[QStringLiteral("result")] = result;
            TestRpcClient::writeReply(peer, 9999, payload);
        });
    });

    client.setEndpoint(QStringLiteral("127.0.0.1"), server.serverPort(), false);
    client.connectToEngine();
    QTRY_VERIFY_WITH_TIMEOUT(RpcClient::State::Ready == client.state(), 3000);

    client.call(QStringLiteral("get_status"), QJsonObject{}, 400);
    QTest::qWait(700);

    // A reply for an unknown id must not be attributed to the pending call.
    QCOMPARE(0, ok.count());
    QCOMPARE(1, failed.count()); // the real call times out
    QCOMPARE(static_cast<int>(RpcErrorKind::Timeout),
             static_cast<int>(failed.takeFirst().at(1).value<RpcError>().kind));

    disconnect(&server, nullptr, &context, nullptr);
}

void TestRpcClient::reconnectsAfterInitialRefusal()
{
    // Bind a port, then close it: the first connect attempt is refused exactly
    // as it is while the launcher's engine is still binding its socket.
    quint16 freePort = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        freePort = probe.serverPort();
        probe.close();
    }

    RpcClient client(QStringLiteral("retry"));
    client.setEndpoint(QStringLiteral("127.0.0.1"), freePort, false);
    client.connectToEngine();

    // Let the refused attempt happen, then bring the "engine" up. The client
    // must retry on its own; a client that only reconnects after a successful
    // connection would stay in Disconnected forever.
    QTest::qWait(400);

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, freePort));

    QTRY_VERIFY_WITH_TIMEOUT(RpcClient::State::Ready == client.state(), 8000);
}

QTEST_MAIN(TestRpcClient)
#include "tst_rpcclient.moc"
