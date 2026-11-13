#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "engine/EngineController.hpp"

using pulse::client::EngineController;

/// Gold Traders wiring, end to end through the real RPC transport.
///
/// A scripted loopback server answers the methods the page polls, so this
/// exercises the parts the pure model tests cannot reach: the array-shaped
/// list_strategies path, selection, the on-demand parameter fetch and the
/// rapid A→B switch that must not let A's late reply overwrite B.
class TestEngineController : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void goldTradersAndSelection();

private:
    QTemporaryDir m_settingsDir;
};

void TestEngineController::initTestCase()
{
    // Keep QSettings away from the operator's real dashboard preferences.
    QVERIFY(m_settingsDir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       m_settingsDir.path());

    // The auto-connect path would race the explicit connect below.
    qunsetenv("PULSE_CLIENT_HOST");
}

/// Build the reply payload for one request. Returns false when the method is
/// unknown, which the test treats as a server bug rather than a client one.
static bool resultFor(const QJsonObject& request, QJsonValue& result)
{
    const QString method = request.value(QStringLiteral("method")).toString();

    if (QStringLiteral("get_status") == method)
    {
        QJsonObject status;
        status[QStringLiteral("version")] = QStringLiteral("test");
        status[QStringLiteral("uptime_sec")] = 1;
        status[QStringLiteral("network")] = QStringLiteral("test");
        status[QStringLiteral("trading_halted")] = false;
        status[QStringLiteral("active_market")] = QStringLiteral("cfd");
        status[QStringLiteral("symbols")] = QJsonArray{ QStringLiteral("XAUUSD") };
        status[QStringLiteral("open_positions")] = 0;
        status[QStringLiteral("strategies_running")] = 2;
        status[QStringLiteral("strategies_total")] = 3;
        result = status;
        return true;
    }
    if (QStringLiteral("get_positions") == method)
    {
        result = QJsonObject{ { QStringLiteral("positions"), QJsonArray{} } };
        return true;
    }
    if (QStringLiteral("get_orders") == method)
    {
        result = QJsonObject{
            { QStringLiteral("activeOrders"), QJsonArray{} },
            { QStringLiteral("recentReports"), QJsonArray{} },
        };
        return true;
    }
    if (QStringLiteral("get_signals") == method)
    {
        result = QJsonObject{
            { QStringLiteral("signals"), QJsonArray{} },
            { QStringLiteral("aggregate"), QJsonValue::Null },
        };
        return true;
    }
    if (QStringLiteral("get_account") == method)
    {
        result = QJsonObject{
            { QStringLiteral("available"), false },
            { QStringLiteral("spot"), QJsonObject{} },
            { QStringLiteral("cfd"), QJsonObject{} },
            { QStringLiteral("spot_balances"), QJsonArray{} },
        };
        return true;
    }
    if (QStringLiteral("list_strategies") == method)
    {
        const auto row = [](const QString& id, const QString& symbol)
        {
            QJsonObject r;
            r[QStringLiteral("id")] = id;
            r[QStringLiteral("name")] = QStringLiteral("MomentumScalper");
            r[QStringLiteral("symbol")] = symbol;
            r[QStringLiteral("enabled")] = true;
            r[QStringLiteral("running")] = true;
            r[QStringLiteral("paused")] = false;
            r[QStringLiteral("auto_trade")] = false;
            r[QStringLiteral("poll_interval_ms")] = 500;
            return r;
        };
        result = QJsonArray{
            row(QStringLiteral("momentum_scalper_XAUUSD"), QStringLiteral("XAUUSD")),
            row(QStringLiteral("mean_reversion_scalper_XAUUSD"), QStringLiteral("XAUUSD")),
            row(QStringLiteral("momentum_scalper_XAGUSD"), QStringLiteral("XAGUSD")),
        };
        return true;
    }
    if (QStringLiteral("get_risk") == method)
    {
        QJsonObject risk;
        risk[QStringLiteral("trading_halted")] = false;
        risk[QStringLiteral("haltReason")] = 0;
        risk[QStringLiteral("dailyDrawdown")] = 0.01;
        risk[QStringLiteral("maxDrawdown")] = 0.02;
        risk[QStringLiteral("rate_limiter_tokens")] = 5.0;
        risk[QStringLiteral("rate_limiter_exhausted")] = false;
        risk[QStringLiteral("portfolio")] = QJsonObject{
            { QStringLiteral("openPositionCount"), 0 },
            { QStringLiteral("total_notional"), 0.0 },
            { QStringLiteral("total_unrealized_pnl"), 0.0 },
            { QStringLiteral("net_exposure"), 0.0 },
        };
        risk[QStringLiteral("openPositionCount")] = 0;
        result = risk;
        return true;
    }
    if (QStringLiteral("get_gold_trader") == method)
    {
        result = QJsonObject{
            { QStringLiteral("armed"), true },
            { QStringLiteral("symbol"), QStringLiteral("XAUUSD") },
            { QStringLiteral("market_type"), QStringLiteral("cfd") },
            { QStringLiteral("config"),
              QJsonObject{
                  { QStringLiteral("hard_stop_usd"), 8.0 },
                  { QStringLiteral("breakeven_trigger_usd"), 2.0 },
                  { QStringLiteral("peak_drawdown_usd"), 5.0 },
                  { QStringLiteral("contract_volume"), 100.0 },
              } },
            { QStringLiteral("tracked_positions"), QJsonArray{} },
        };
        return true;
    }
    if (QStringLiteral("get_strategy_params") == method)
    {
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();
        const QString id = params.value(QStringLiteral("strategy_id")).toString();

        QJsonObject values;
        values[QStringLiteral("order_quantity")] =
            QStringLiteral("momentum_scalper_XAUUSD") == id ? 0.01 : 0.02;
        values[QStringLiteral("min_confidence")] = 0.6;
        values[QStringLiteral("auto_trade")] = 0.0;
        result = values;
        return true;
    }
    return false;
}

void TestEngineController::goldTradersAndSelection()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    // Every accepted socket gets its own line buffer: requests can arrive in
    // fragments and several sockets are attached at once.
    QHash<QTcpSocket*, QByteArray> buffers;
    QObject context;
    connect(&server, &QTcpServer::newConnection, &context, [&server, &buffers, &context]()
    {
        while (server.hasPendingConnections())
        {
            QTcpSocket* peer = server.nextPendingConnection();
            buffers.insert(peer, QByteArray{});
            connect(peer, &QTcpSocket::readyRead, &context, [peer, &buffers]()
            {
                QByteArray& buffer = buffers[peer];
                buffer.append(peer->readAll());

                int newline = buffer.indexOf('\n');
                while (newline >= 0)
                {
                    const QByteArray line = buffer.left(newline);
                    buffer.remove(0, newline + 1);
                    newline = buffer.indexOf('\n');
                    if (line.trimmed().isEmpty())
                    {
                        continue;
                    }

                    const QJsonObject request = QJsonDocument::fromJson(line).object();
                    QJsonValue result;
                    QJsonObject reply;
                    reply[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
                    reply[QStringLiteral("id")] = request.value(QStringLiteral("id"));
                    if (resultFor(request, result))
                    {
                        reply[QStringLiteral("result")] = result;
                    }
                    else
                    {
                        reply[QStringLiteral("error")] = QJsonObject{
                            { QStringLiteral("code"), -32601 },
                            { QStringLiteral("message"),
                              QStringLiteral("method not found: ")
                                  + request.value(QStringLiteral("method")).toString() },
                        };
                    }
                    peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                    peer->flush();
                }
            });
        }
    });

    EngineController controller;
    controller.connectToEngine(QStringLiteral("127.0.0.1"), server.serverPort(),
                               false, QString());

    // The array-shaped list_strategies result must reach the model; a
    // toObject()-only transport would leave this at zero forever.
    QTRY_VERIFY_WITH_TIMEOUT(2 == controller.goldStrategies()->count(), 5000);
    QCOMPARE(3, controller.strategiesModel()->count());
    QTRY_VERIFY_WITH_TIMEOUT(controller.riskModel()->present(), 5000);
    QCOMPARE(0.01, controller.riskModel()->dailyDrawdown());

    // The L7 status card gets its data from get_gold_trader.
    QTRY_VERIFY_WITH_TIMEOUT(controller.goldTrader().value(QStringLiteral("armed")).toBool(), 5000);
    QCOMPARE(QStringLiteral("XAUUSD"),
             controller.goldTrader().value(QStringLiteral("symbol")).toString());
    QCOMPARE(8.0,
             controller.goldTrader()
                 .value(QStringLiteral("config"))
                 .toMap()
                 .value(QStringLiteral("hard_stop_usd"))
                 .toDouble());

    // Selecting a trader fetches its parameters on demand.
    controller.selectStrategy(QStringLiteral("momentum_scalper_XAUUSD"));
    QTRY_VERIFY_WITH_TIMEOUT(controller.strategyParamsModel()->present(), 5000);
    QCOMPARE(QStringLiteral("momentum_scalper_XAUUSD"),
             controller.strategyParamsModel()->strategyId());
    QCOMPARE(3, controller.strategyParamsModel()->count());
    QCOMPARE(QStringLiteral("MomentumScalper"),
             controller.selectedStrategy().value(QStringLiteral("name")).toString());

    // Rapid A→B: A's in-flight reply must not overwrite B. The scripted server
    // answers immediately, so both selects run before the event loop delivers
    // the first reply — exactly the deferral path in selectStrategy().
    controller.selectStrategy(QStringLiteral("momentum_scalper_XAUUSD"));
    controller.selectStrategy(QStringLiteral("mean_reversion_scalper_XAUUSD"));
    QTRY_VERIFY_WITH_TIMEOUT(
        controller.strategyParamsModel()->present()
            && QStringLiteral("mean_reversion_scalper_XAUUSD")
                   == controller.strategyParamsModel()->strategyId(),
        5000);
    QCOMPARE(0.02, controller.strategyParamsModel()
                       ->params().at(0).toMap()
                       .value(QStringLiteral("value")).toDouble());

    controller.disconnectFromEngine();
}

QTEST_MAIN(TestEngineController)
#include "tst_enginecontroller.moc"