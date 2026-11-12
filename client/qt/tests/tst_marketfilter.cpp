// Market filter: one polled snapshot, three market views.
//
// Guards the dashboard contract: a spot row must never appear on the CFD or
// futures tab, and the count property that drives the tab labels must follow
// every change to the visible set.

#include "models/JsonListModel.hpp"
#include "models/MarketFilterModel.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

using namespace pulse::client;

namespace
{

QJsonObject row(const QString& id, const QString& symbol, const QString& market)
{
    return QJsonObject{
        { QStringLiteral("position_id"), id },
        { QStringLiteral("symbol"), symbol },
        { QStringLiteral("market_type"), market },
    };
}

/// Two spot positions, one futures position, no CFD position.
QJsonArray sampleRows()
{
    return QJsonArray{
        row(QStringLiteral("p1"), QStringLiteral("BTC_USDT"), QStringLiteral("spot")),
        row(QStringLiteral("p2"), QStringLiteral("ETH_USDT"), QStringLiteral("futures")),
        row(QStringLiteral("p3"), QStringLiteral("SOL_USDT"), QStringLiteral("spot")),
    };
}

} // namespace

class MarketFilterTest : public QObject
{
    Q_OBJECT

private slots:
    void filtersByMarketType();
    void emptyFilterAcceptsAll();
    void unknownMarketRejectsAll();
    void switchingMarketUpdatesCount();
    void sourceResetUpdatesCount();
    void nullSourceAcceptsNothing();
};

void MarketFilterTest::filtersByMarketType()
{
    JsonListModel source(QStringLiteral("position_id"));
    source.setRows(sampleRows());

    MarketFilterModel filter;
    filter.setSourceModel(&source);
    filter.setMarketType(QStringLiteral("spot"));

    QCOMPARE(2, filter.count());
    QCOMPARE(2, filter.rowCount());

    const int symbolRole = filter.roleNames().key("symbol", -1);
    QVERIFY(symbolRole >= 0);
    QCOMPARE(QStringLiteral("BTC_USDT"),
             filter.data(filter.index(0, 0), symbolRole).toString());
    QCOMPARE(QStringLiteral("SOL_USDT"),
             filter.data(filter.index(1, 0), symbolRole).toString());

    filter.setMarketType(QStringLiteral("futures"));
    QCOMPARE(1, filter.count());
    QCOMPARE(QStringLiteral("ETH_USDT"),
             filter.data(filter.index(0, 0), symbolRole).toString());
}

void MarketFilterTest::emptyFilterAcceptsAll()
{
    JsonListModel source(QStringLiteral("position_id"));
    source.setRows(sampleRows());

    MarketFilterModel filter;
    filter.setSourceModel(&source);

    // Empty is the "All" segment.
    QCOMPARE(3, filter.count());
}

void MarketFilterTest::unknownMarketRejectsAll()
{
    JsonListModel source(QStringLiteral("position_id"));
    source.setRows(sampleRows());

    MarketFilterModel filter;
    filter.setSourceModel(&source);
    filter.setMarketType(QStringLiteral("margin"));

    QCOMPARE(0, filter.count());
}

void MarketFilterTest::switchingMarketUpdatesCount()
{
    JsonListModel source(QStringLiteral("position_id"));
    source.setRows(sampleRows());

    MarketFilterModel filter;
    filter.setSourceModel(&source);
    filter.setMarketType(QStringLiteral("spot"));

    QSignalSpy spy(&filter, &MarketFilterModel::countChanged);
    QVERIFY(spy.isValid());

    filter.setMarketType(QStringLiteral("cfd"));
    QCOMPARE(0, filter.count());
    QVERIFY(spy.count() >= 1);

    // Setting the same value again is a no-op (no duplicate notifications).
    const int before = spy.count();
    filter.setMarketType(QStringLiteral("cfd"));
    QCOMPARE(before, spy.count());
}

void MarketFilterTest::sourceResetUpdatesCount()
{
    JsonListModel source(QStringLiteral("position_id"));
    source.setRows(sampleRows());

    MarketFilterModel filter;
    filter.setSourceModel(&source);
    filter.setMarketType(QStringLiteral("spot"));
    QCOMPARE(2, filter.count());

    // A new key sequence resets the source model; the proxy must re-evaluate.
    source.setRows(QJsonArray{
        row(QStringLiteral("p9"), QStringLiteral("XAUUSD"), QStringLiteral("cfd")),
        row(QStringLiteral("p10"), QStringLiteral("XAGUSD"), QStringLiteral("cfd")),
    });
    QCOMPARE(0, filter.count());

    filter.setMarketType(QStringLiteral("cfd"));
    QCOMPARE(2, filter.count());
}

void MarketFilterTest::nullSourceAcceptsNothing()
{
    MarketFilterModel filter;
    filter.setMarketType(QStringLiteral("spot"));

    // Before the first payload (or after a disconnect) there is nothing to
    // show; the tab must render empty rather than borrowing another market.
    QCOMPARE(0, filter.count());
}

QTEST_MAIN(MarketFilterTest)
#include "tst_marketfilter.moc"