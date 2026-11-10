// tst_accountmodel.cpp — AccountModel mapping of the `get_account` payload.
//
// Regression: the model used to read futures fields from a nested "futures"
// object, while the engine reports them at the TOP LEVEL of the result
// (docs/qt-client-design.md §3.2). The blocks were therefore always absent and
// the dashboard silently hid the futures tiles on every machine.
//
// Only parsing is exercised here: JSON in, properties out, no sockets.

#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>

#include "models/AccountModel.hpp"

using pulse::client::AccountModel;

namespace
{
/// Parse a JSON literal into the object shape the engine sends.
QJsonObject parseAccount(const char* text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}
} // namespace

class TestAccountModel : public QObject
{
    Q_OBJECT

private slots:
    void futuresFieldsComeFromTheTopLevel();
    void futuresAbsentWhenTheCallFailed();
    void spotBlockIsParsed();
    void cfdBlockIsParsed();
    void absentBlocksClearPresence();
};

void TestAccountModel::futuresFieldsComeFromTheTopLevel()
{
    AccountModel model;
    model.setFromJson(parseAccount(R"({
        "available": true,
        "total": 556.49,
        "available_balance": 512.34,
        "unrealised_pnl": 0.52,
        "position_margin": 44.15,
        "order_margin": 1.25,
        "currency": "USDT"
    })"));

    QVERIFY(model.futuresPresent());
    QCOMPARE(model.futuresCurrency(), QStringLiteral("USDT"));
    QCOMPARE(model.futuresTotal(), 556.49);
    QCOMPARE(model.futuresAvailable(), 512.34);
    QCOMPARE(model.futuresUnrealised(), 0.52);
    QCOMPARE(model.futuresPositionMargin(), 44.15);
    QCOMPARE(model.futuresOrderMargin(), 1.25);
}

void TestAccountModel::futuresAbsentWhenTheCallFailed()
{
    // `available:false` means the futures REST call did not succeed. Any stale
    // numbers in the payload must not put a balance on screen.
    AccountModel model;
    model.setFromJson(parseAccount(R"({
        "available": false,
        "total": 0.0,
        "currency": "USDT"
    })"));

    QVERIFY(!model.futuresPresent());
}

void TestAccountModel::spotBlockIsParsed()
{
    AccountModel model;
    model.setFromJson(parseAccount(R"({
        "available": false,
        "spot": {"available": 250.0, "total": 260.0, "currency": "USDT"},
        "spot_balances": [
            {"currency": "USDT", "available": 250.0, "locked": 10.0,
             "total": 260.0, "value_usdt": 260.0},
            {"currency": "BTC", "available": 0.001, "locked": 0.0,
             "total": 0.001, "value_usdt": null}
        ]
    })"));

    QVERIFY(!model.futuresPresent());
    QVERIFY(model.spotPresent());
    QCOMPARE(model.spotCurrency(), QStringLiteral("USDT"));
    QCOMPARE(model.spotTotal(), 260.0);
    QCOMPARE(model.spotAvailable(), 250.0);

    const QVariantList holdings = model.spotBalances();
    QCOMPARE(holdings.size(), 2);
    // A null value_usdt means "no ticker cached", not zero.
    QVERIFY(holdings.at(1).toMap().value(QStringLiteral("hasValue")).toBool() == false);
}

void TestAccountModel::cfdBlockIsParsed()
{
    AccountModel model;
    model.setFromJson(parseAccount(R"({
        "cfd": {
            "total": 1000.5,
            "available": 900.25,
            "margin": 100.25,
            "unrealised_pnl": -5.5,
            "currency": "USD"
        }
    })"));

    QVERIFY(model.cfdPresent());
    QCOMPARE(model.cfdCurrency(), QStringLiteral("USD"));
    QCOMPARE(model.cfdTotal(), 1000.5);
    QCOMPARE(model.cfdAvailable(), 900.25);
    QCOMPARE(model.cfdMargin(), 100.25);
    QCOMPARE(model.cfdUnrealised(), -5.5);
}

void TestAccountModel::absentBlocksClearPresence()
{
    AccountModel model;
    model.setFromJson(parseAccount(R"({
        "available": true,
        "total": 10.0,
        "currency": "USDT",
        "spot": {"available": 1.0, "total": 1.0, "currency": "USDT"},
        "cfd": {"total": 2.0, "available": 2.0, "margin": 0.0,
                "unrealised_pnl": 0.0, "currency": "USD"}
    })"));
    QVERIFY(model.futuresPresent());
    QVERIFY(model.spotPresent());
    QVERIFY(model.cfdPresent());

    // A later snapshot without the blocks must not leave stale values on
    // screen: presence flags drive rendering.
    model.setFromJson(parseAccount(R"({"available": false})"));
    QVERIFY(!model.futuresPresent());
    QVERIFY(!model.spotPresent());
    QVERIFY(!model.cfdPresent());
}

QTEST_MAIN(TestAccountModel)
#include "tst_accountmodel.moc"
