#include <QtTest>

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "market/MarketStream.hpp"

using pulse::client::MarketStream;

/// Scripted control socket: one loopback listener that answers get_market with
/// a configurable ticker. No engine process and no real network are involved.
class FakeEngine : public QObject
{
    Q_OBJECT

  public:
    explicit FakeEngine(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this,
                &FakeEngine::onNewConnection);
    }

    ~FakeEngine() override
    {
        close();
    }

    [[nodiscard]] bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    [[nodiscard]] quint16 port() const
    {
        return m_server.serverPort();
    }

    void setPrice(double price)
    {
        m_price = price;
    }

    void setError(const QString& error)
    {
        m_error = error;
    }

    /// Shift the ticker timestamp into the past (positive value) so the
    /// freshness guard can be exercised.
    void setTimestampAgeMs(qint64 ageMs)
    {
        m_timestampAgeMs = ageMs;
    }

    /// Answer with a kline but no ticker, which exercises the display-only
    /// fallback path.
    void setKlineOnly(bool enabled)
    {
        m_klineOnly = enabled;
    }

    [[nodiscard]] int requestCount() const
    {
        return m_requestCount;
    }

    [[nodiscard]] QJsonObject lastParams() const
    {
        return m_lastParams;
    }

    void close()
    {
        m_server.close();
        qDeleteAll(m_clients);
        m_clients.clear();
        m_buffers.clear();
    }

  private:
    void onNewConnection()
    {
        while (m_server.hasPendingConnections())
        {
            QTcpSocket* socket = m_server.nextPendingConnection();
            m_clients.append(socket);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]()
            {
                onReadyRead(socket);
            });
        }
    }

    void onReadyRead(QTcpSocket* socket)
    {
        QByteArray& buffer = m_buffers[socket];
        buffer.append(socket->readAll());

        int newline = -1;
        while (0 <= (newline = buffer.indexOf('\n')))
        {
            const QByteArray line = buffer.left(newline);
            buffer.remove(0, newline + 1);

            QJsonParseError parseError;
            const QJsonDocument document =
                QJsonDocument::fromJson(line, &parseError);
            if (QJsonParseError::NoError != parseError.error
                || !document.isObject())
            {
                continue;
            }
            respond(socket, document.object());
        }
    }

    void respond(QTcpSocket* socket, const QJsonObject& request)
    {
        QJsonObject reply;
        reply[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        reply[QStringLiteral("id")] = request.value(QStringLiteral("id"));

        if (QStringLiteral("get_market")
            != request.value(QStringLiteral("method")).toString())
        {
            QJsonObject error;
            error[QStringLiteral("code")] = -32601;
            error[QStringLiteral("message")] = QStringLiteral("unknown method");
            reply[QStringLiteral("error")] = error;
        }
        else
        {
            ++m_requestCount;
            m_lastParams = request.value(QStringLiteral("params")).toObject();

            QJsonObject result;
            if (!m_error.isEmpty())
            {
                // The engine reports logical failures as a top-level "error"
                // inside a successful JSON-RPC result.
                result[QStringLiteral("error")] = m_error;
            }
            else if (m_klineOnly)
            {
                QJsonObject kline;
                kline[QStringLiteral("open_time")] =
                    static_cast<double>(QDateTime::currentMSecsSinceEpoch()
                                        - 60'000);
                kline[QStringLiteral("close")] = m_price;
                QJsonArray klines;
                klines.append(kline);
                result[QStringLiteral("symbol")] = QStringLiteral("BTC_USDT");
                result[QStringLiteral("klines")] = klines;
            }
            else
            {
                QJsonObject ticker;
                ticker[QStringLiteral("symbol")] = QStringLiteral("BTC_USDT");
                ticker[QStringLiteral("last")] = m_price;
                ticker[QStringLiteral("bid")] = m_price - 0.1;
                ticker[QStringLiteral("ask")] = m_price + 0.1;
                ticker[QStringLiteral("change_pct")] = 1.25;
                ticker[QStringLiteral("volume_24h")] = 1234.5;
                ticker[QStringLiteral("timestamp")] =
                    static_cast<double>(QDateTime::currentMSecsSinceEpoch()
                                        - m_timestampAgeMs);
                result[QStringLiteral("symbol")] = QStringLiteral("BTC_USDT");
                result[QStringLiteral("ticker")] = ticker;
            }
            reply[QStringLiteral("result")] = result;
        }

        QByteArray line = QJsonDocument(reply).toJson(QJsonDocument::Compact);
        line.append('\n');
        socket->write(line);
        socket->flush();
    }

    QTcpServer m_server;
    QList<QTcpSocket*> m_clients;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    double m_price = 100.0;
    QString m_error;
    qint64 m_timestampAgeMs = 0;
    bool m_klineOnly = false;
    int m_requestCount = 0;
    QJsonObject m_lastParams;
};

/// Live stream behaviour: one request per poll, OHLC folding, freshness and
/// stop semantics. The fake engine is deterministic apart from the wall clock.
class TestMarketStream : public QObject
{
    Q_OBJECT

  private slots:
    void defaultsAreOneHertzAndTenSeconds();
    void pollsAndBuildsCandles();
    void resumeKeepsCandles();
    void reportsEngineErrors();
    void staleTickerIsNotFolded();
    void klineFallbackIsDisplayOnly();
    void stopHaltsPolling();
};

void TestMarketStream::defaultsAreOneHertzAndTenSeconds()
{
    MarketStream stream;
    QCOMPARE(stream.pollIntervalMs(), 1000);
    QCOMPARE(stream.candlePeriodSec(), 10);
    QVERIFY(!stream.streaming());
    QVERIFY(stream.candles().isEmpty());
}

void TestMarketStream::pollsAndBuildsCandles()
{
    FakeEngine engine;
    QVERIFY(engine.listen());

    MarketStream stream;
    stream.setPollIntervalMs(50);
    stream.setCandlePeriodSec(1);
    stream.setEndpoint(QStringLiteral("127.0.0.1"), engine.port(), false,
                       QString());

    QSignalSpy tickerSpy(&stream, &MarketStream::tickerChanged);
    QSignalSpy candleSpy(&stream, &MarketStream::candlesChanged);
    stream.start(QStringLiteral("BTC_USDT"), QStringLiteral("spot"));

    QTRY_VERIFY_WITH_TIMEOUT(0.0 < stream.lastPrice(), 3000);
    QCOMPARE(stream.lastPrice(), 100.0);
    QVERIFY(stream.engineReady());
    QVERIFY(!stream.stale());
    QVERIFY(stream.streaming());
    QVERIFY(0 < stream.sampleCount());

    const QJsonObject params = engine.lastParams();
    QCOMPARE(params.value(QStringLiteral("symbol")).toString(),
             QStringLiteral("BTC_USDT"));
    QCOMPARE(params.value(QStringLiteral("market_type")).toString(),
             QStringLiteral("spot"));
    QCOMPARE(params.value(QStringLiteral("book_levels")).toInt(), 0);
    QCOMPARE(params.value(QStringLiteral("klines")).toInt(), 1);

    // The first candle opens at the first sample's price.
    QCOMPARE(stream.candles().size(), 1);
    const QVariantMap firstCandle = stream.candles().first().toMap();
    QCOMPARE(firstCandle.value(QStringLiteral("o")).toDouble(), 100.0);
    QCOMPARE(firstCandle.value(QStringLiteral("n")).toInt(), stream.sampleCount());

    // A new 1-second bucket must close the first candle and open another.
    engine.setPrice(101.0);
    QTRY_VERIFY_WITH_TIMEOUT(1 < stream.candles().size(), 4000);
    QVERIFY(stream.candles().first().toMap()
                .value(QStringLiteral("closed")).toBool());
    QCOMPARE(stream.candles().last().toMap()
                 .value(QStringLiteral("o")).toDouble(), 101.0);

    QVERIFY(0 < tickerSpy.count());
    QVERIFY(0 < candleSpy.count());
    stream.stop();
}

void TestMarketStream::reportsEngineErrors()
{
    FakeEngine engine;
    QVERIFY(engine.listen());
    engine.setError(QStringLiteral("no market feed available"));

    MarketStream stream;
    stream.setPollIntervalMs(50);
    stream.setEndpoint(QStringLiteral("127.0.0.1"), engine.port(), false,
                       QString());

    stream.start(QStringLiteral("BTC_USDT"), QStringLiteral("spot"));

    QTRY_VERIFY_WITH_TIMEOUT(!stream.lastError().isEmpty(), 3000);
    QCOMPARE(stream.lastError(), QStringLiteral("no market feed available"));
    QVERIFY(!stream.engineReady());
    QVERIFY(stream.stale());
    QCOMPARE(stream.candles().size(), 0);
    stream.stop();
    QVERIFY(stream.lastError().isEmpty());
}

void TestMarketStream::resumeKeepsCandles()
{
    FakeEngine engine;
    QVERIFY(engine.listen());

    MarketStream stream;
    stream.setPollIntervalMs(50);
    stream.setCandlePeriodSec(1);
    stream.setEndpoint(QStringLiteral("127.0.0.1"), engine.port(), false,
                       QString());

    stream.start(QStringLiteral("BTC_USDT"), QStringLiteral("spot"));
    QTRY_VERIFY_WITH_TIMEOUT(!stream.candles().isEmpty(), 3000);

    const int collected = stream.candles().size();
    stream.stop();
    QVERIFY(!stream.streaming());

    // The page hides/stops and comes back: the same instrument must resume
    // without discarding the chart.
    stream.start(QStringLiteral("BTC_USDT"), QStringLiteral("spot"));
    QVERIFY(stream.streaming());
    QCOMPARE(stream.candles().size(), collected);
    stream.stop();
}

void TestMarketStream::staleTickerIsNotFolded()
{
    FakeEngine engine;
    QVERIFY(engine.listen());
    // The exchange timestamp is 60 s old: the price is still displayed, but it
    // must not become a candle sample.
    engine.setTimestampAgeMs(60'000);

    MarketStream stream;
    stream.setPollIntervalMs(50);
    stream.setCandlePeriodSec(1);
    stream.setEndpoint(QStringLiteral("127.0.0.1"), engine.port(), false,
                       QString());

    stream.start(QStringLiteral("BTC_USDT"), QStringLiteral("spot"));

    QTRY_VERIFY_WITH_TIMEOUT(0.0 < stream.lastPrice(), 3000);
    QVERIFY(stream.engineReady());
    QVERIFY(stream.stale());
    QCOMPARE(stream.candles().size(), 0);
    QCOMPARE(stream.sampleCount(), 0);
    stream.stop();
}

void TestMarketStream::klineFallbackIsDisplayOnly()
{
    FakeEngine engine;
    QVERIFY(engine.listen());
    engine.setKlineOnly(true);
    engine.setPrice(123.0);

    MarketStream stream;
    stream.setPollIntervalMs(50);
    stream.setCandlePeriodSec(1);
    stream.setEndpoint(QStringLiteral("127.0.0.1"), engine.port(), false,
                       QString());

    stream.start(QStringLiteral("BTC_USDT"), QStringLiteral("spot"));

    // The 1-minute kline close is shown, but it is not a live trade sample:
    // no candle may be fabricated from it.
    QTRY_VERIFY_WITH_TIMEOUT(0.0 < stream.lastPrice(), 3000);
    QCOMPARE(stream.lastPrice(), 123.0);
    QVERIFY(stream.engineReady());
    QVERIFY(stream.stale());
    QCOMPARE(stream.candles().size(), 0);
    QCOMPARE(stream.sampleCount(), 0);
    stream.stop();
}

void TestMarketStream::stopHaltsPolling()
{
    FakeEngine engine;
    QVERIFY(engine.listen());

    MarketStream stream;
    stream.setPollIntervalMs(50);
    stream.setEndpoint(QStringLiteral("127.0.0.1"), engine.port(), false,
                       QString());

    stream.start(QStringLiteral("BTC_USDT"), QStringLiteral("spot"));
    QTRY_VERIFY_WITH_TIMEOUT(0.0 < stream.lastPrice(), 3000);

    stream.stop();
    QVERIFY(!stream.streaming());

    // Let an in-flight reply land before sampling the counter.
    QTest::qWait(150);
    const int requests = engine.requestCount();
    QTest::qWait(300);
    QCOMPARE(engine.requestCount(), requests);
}

QTEST_MAIN(TestMarketStream)

#include "tst_marketstream.moc"