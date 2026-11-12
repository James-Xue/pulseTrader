#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>

#include "models/JsonListModel.hpp"
#include "models/RiskModel.hpp"
#include "models/StrategiesModel.hpp"
#include "models/StrategyParamsModel.hpp"
#include "models/SymbolFilterModel.hpp"

using pulse::client::JsonListModel;
using pulse::client::RiskModel;
using pulse::client::StrategiesModel;
using pulse::client::StrategyParamsModel;
using pulse::client::SymbolFilterModel;

/// Gold Traders page models.
///
/// These classes are pure JSON-in / properties-out: no sockets, no engine and
/// no QSettings. The tests pin the two failure modes the page cannot recover
/// from on its own — a lookalike symbol leaking into the gold list, and a
/// stale parameter reply overwriting a newer selection.
class TestGoldTraders : public QObject
{
    Q_OBJECT

private slots:
    void strategiesModelIsKeyedOnId();
    void goldFilterKeepsOnlyXauusd();
    void goldFilterExcludesLookalikeSymbols();
    void goldFilterRejectsBeforeFirstPayload();
    void riskModelFlattensSnapshot();
    void riskModelTreatsAbsenceAsUnknown();
    void paramsModelKeepsCanonicalOrder();
    void paramsModelDropsStaleSelection();
    void paramsModelHandlesPending();

private:
    static QJsonObject strategyRow(const QString& id, const QString& symbol,
                                   bool paused = false, bool autoTrade = false)
    {
        QJsonObject row;
        row[QStringLiteral("id")] = id;
        row[QStringLiteral("name")] = QStringLiteral("MomentumScalper");
        row[QStringLiteral("symbol")] = symbol;
        row[QStringLiteral("enabled")] = true;
        row[QStringLiteral("running")] = true;
        row[QStringLiteral("paused")] = paused;
        row[QStringLiteral("auto_trade")] = autoTrade;
        row[QStringLiteral("poll_interval_ms")] = 500;
        return row;
    }
};

void TestGoldTraders::strategiesModelIsKeyedOnId()
{
    StrategiesModel model;
    const QJsonArray rows = {
        strategyRow(QStringLiteral("momentum_scalper_XAUUSD"), QStringLiteral("XAUUSD")),
        strategyRow(QStringLiteral("supertrend_scalper_XAUUSD"), QStringLiteral("XAUUSD")),
    };
    model.setRows(rows);
    QCOMPARE(2, model.count());

    // Roles are discovered from the first row; QML reads these names.
    const QHash<int, QByteArray> roles = model.roleNames();
    QVERIFY(roles.values().contains(QByteArrayLiteral("id")));
    QVERIFY(roles.values().contains(QByteArrayLiteral("name")));
    QVERIFY(roles.values().contains(QByteArrayLiteral("symbol")));
    QVERIFY(roles.values().contains(QByteArrayLiteral("auto_trade")));

    // Same key sequence: updated in place, so a model reset does not clear the
    // page's selected id.
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    const QJsonArray updated = {
        strategyRow(QStringLiteral("momentum_scalper_XAUUSD"), QStringLiteral("XAUUSD"), true),
        strategyRow(QStringLiteral("supertrend_scalper_XAUUSD"), QStringLiteral("XAUUSD")),
    };
    model.setRows(updated);
    QCOMPARE(0, resetSpy.count());

    const int pausedRole = model.roleNames().key(QByteArrayLiteral("paused"));
    QCOMPARE(true, model.data(model.index(0, 0), pausedRole).toBool());
}

void TestGoldTraders::goldFilterKeepsOnlyXauusd()
{
    JsonListModel source(QStringLiteral("id"));
    SymbolFilterModel filter;
    filter.setSourceModel(&source);
    filter.setSymbol(QStringLiteral("XAUUSD"));

    source.setRows({
        strategyRow(QStringLiteral("momentum_scalper_XAUUSD"), QStringLiteral("XAUUSD")),
        strategyRow(QStringLiteral("supertrend_scalper_XAUUSD"), QStringLiteral("XAUUSD")),
        strategyRow(QStringLiteral("momentum_scalper_BTC_USDT"), QStringLiteral("BTC_USDT")),
        strategyRow(QStringLiteral("momentum_scalper_XAGUSD"), QStringLiteral("XAGUSD")),
    });

    // Silver is a different instrument even though it is a CFD.
    QCOMPARE(2, filter.count());
}

void TestGoldTraders::goldFilterExcludesLookalikeSymbols()
{
    JsonListModel source(QStringLiteral("id"));
    SymbolFilterModel filter;
    filter.setSourceModel(&source);
    filter.setSymbol(QStringLiteral("XAUUSD"));

    source.setRows({
        strategyRow(QStringLiteral("gold_futures_XAU_USDT"), QStringLiteral("XAU_USDT")),
        strategyRow(QStringLiteral("gold_token_XAUT_USDT"), QStringLiteral("XAUT_USDT")),
        strategyRow(QStringLiteral("silver_XAGUSD"), QStringLiteral("XAGUSD")),
    });

    // A contains()/startsWith("XAU") refactor would make this 3 and silently
    // show futures, tokenized and silver instances as gold traders.
    QCOMPARE(0, filter.count());
}

void TestGoldTraders::goldFilterRejectsBeforeFirstPayload()
{
    JsonListModel source(QStringLiteral("id"));
    SymbolFilterModel filter;
    filter.setSourceModel(&source);
    filter.setSymbol(QStringLiteral("XAUUSD"));

    // The proxy is queried before JsonListModel has registered any role.
    QCOMPARE(0, filter.count());

    source.setRows({
        strategyRow(QStringLiteral("momentum_scalper_XAUUSD"), QStringLiteral("XAUUSD")),
    });
    QCOMPARE(1, filter.count());
}

void TestGoldTraders::riskModelFlattensSnapshot()
{
    QJsonObject portfolio;
    portfolio[QStringLiteral("openPositionCount")] = 2;
    portfolio[QStringLiteral("total_notional")] = 123.4;
    portfolio[QStringLiteral("total_unrealized_pnl")] = -5.6;
    portfolio[QStringLiteral("net_exposure")] = 100.0;

    QJsonObject risk;
    risk[QStringLiteral("trading_halted")] = true;
    risk[QStringLiteral("haltReason")] = 3001;
    risk[QStringLiteral("dailyDrawdown")] = 0.05;
    risk[QStringLiteral("maxDrawdown")] = 0.12;
    risk[QStringLiteral("rate_limiter_tokens")] = 3.0;
    risk[QStringLiteral("rate_limiter_exhausted")] = false;
    risk[QStringLiteral("portfolio")] = portfolio;
    risk[QStringLiteral("openPositionCount")] = 2;

    RiskModel model;
    model.setFromJson(risk);

    QVERIFY(model.present());
    QVERIFY(model.tradingHalted());
    QCOMPARE(3001, model.haltReasonCode());
    // Drawdowns stay fractions; the page multiplies for display.
    QCOMPARE(0.05, model.dailyDrawdown());
    QCOMPARE(0.12, model.maxDrawdown());
    QCOMPARE(3.0, model.rateLimiterTokens());
    QVERIFY(!model.rateLimiterExhausted());
    QVERIFY(model.portfolioPresent());
    QCOMPARE(2, model.portfolioOpenPositions());
    QCOMPARE(123.4, model.portfolioNotional());
    QCOMPARE(-5.6, model.portfolioUnrealizedPnl());
    QCOMPARE(100.0, model.portfolioNetExposure());
    QCOMPARE(2, model.openPositionCount());
}

void TestGoldTraders::riskModelTreatsAbsenceAsUnknown()
{
    RiskModel model;
    model.setFromJson({});

    // The page gates every tile on `present`, so a missing snapshot must not
    // render as a healthy zero.
    QVERIFY(!model.present());
    QVERIFY(!model.portfolioPresent());
    QVERIFY(!model.tradingHalted());

    // A partial snapshot is still present, but the absent portfolio block
    // stays hidden rather than zeroed.
    QJsonObject partial;
    partial[QStringLiteral("trading_halted")] = false;
    partial[QStringLiteral("dailyDrawdown")] = 0.01;
    model.setFromJson(partial);
    QVERIFY(model.present());
    QVERIFY(!model.portfolioPresent());
    QCOMPARE(0.01, model.dailyDrawdown());
}

void TestGoldTraders::paramsModelKeepsCanonicalOrder()
{
    StrategyParamsModel model;
    model.setPending(QStringLiteral("a"));

    QJsonObject params;
    // Inserted in reverse reading order on purpose.
    params[QStringLiteral("zeta_custom")] = 9.0;
    params[QStringLiteral("auto_trade")] = 0.0;
    params[QStringLiteral("take_profit_pct")] = 2.0;
    params[QStringLiteral("order_quantity")] = 0.01;

    model.setFromJson(QStringLiteral("a"), params);
    QVERIFY(model.present());
    QVERIFY(!model.pending());
    QCOMPARE(4, model.count());

    const QVariantList rows = model.params();
    QCOMPARE(QStringLiteral("order_quantity"), rows.at(0).toMap().value(QStringLiteral("key")).toString());
    QCOMPARE(QStringLiteral("take_profit_pct"), rows.at(1).toMap().value(QStringLiteral("key")).toString());
    QCOMPARE(QStringLiteral("auto_trade"), rows.at(2).toMap().value(QStringLiteral("key")).toString());
    // Unknown keys are appended instead of being dropped.
    QCOMPARE(QStringLiteral("zeta_custom"), rows.at(3).toMap().value(QStringLiteral("key")).toString());
}

void TestGoldTraders::paramsModelDropsStaleSelection()
{
    StrategyParamsModel model;
    model.setPending(QStringLiteral("a"));

    QJsonObject params;
    params[QStringLiteral("order_quantity")] = 0.01;
    model.setFromJson(QStringLiteral("a"), params);
    QVERIFY(model.present());

    // A late failure for a previous selection must not paint over A.
    model.setError(QStringLiteral("b"), QStringLiteral("strategy not found"));
    QCOMPARE(QStringLiteral("a"), model.strategyId());
    QVERIFY(model.present());
    QVERIFY(model.error().isEmpty());
    QCOMPARE(1, model.count());

    // A reply for the new selection replaces it.
    model.setPending(QStringLiteral("b"));
    model.setFromJson(QStringLiteral("b"),
                      QJsonObject{ { QStringLiteral("order_quantity"), 0.02 } });
    QCOMPARE(QStringLiteral("b"), model.strategyId());
    QCOMPARE(1, model.count());
    QCOMPARE(0.02, model.params().at(0).toMap().value(QStringLiteral("value")).toDouble());
}

void TestGoldTraders::paramsModelHandlesPending()
{
    StrategyParamsModel model;
    model.setPending(QStringLiteral("a"));

    QVERIFY(model.pending());
    QVERIFY(!model.present());
    QVERIFY(model.error().isEmpty());
    QCOMPARE(0, model.count());

    model.setFromJson(QStringLiteral("a"),
                      QJsonObject{ { QStringLiteral("order_quantity"), 0.01 } });
    QVERIFY(!model.pending());
    QVERIFY(model.present());

    model.clear();
    QVERIFY(!model.present());
    QVERIFY(!model.pending());
    QVERIFY(model.strategyId().isEmpty());
    QCOMPARE(0, model.count());
}

QTEST_MAIN(TestGoldTraders)
#include "tst_goldtraders.moc"