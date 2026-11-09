#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace pulse::client::data
{

/// Wire version carried by the hello/welcome exchange. A consumer that speaks
/// a different number is rejected with -32004, so bumping this is a breaking
/// change for every downstream reader.
inline constexpr int kProtocolVersion = 1;

/// Periods and smoothing constants of every indicator the server publishes.
///
/// The welcome frame advertises this exact set, so the consumer can validate
/// its own math against the server instead of hard-coding a guess. Changing a
/// value here is protocol-visible.
struct IndicatorParams
{
    QList<int> emaPeriods{ 7, 14, 30, 60, 200 };
    int macdFast = 12;
    int macdSlow = 26;
    int macdSignal = 9;
    int atrPeriod = 10;
    double supertrendMultiplier = 3.0;

    /// Serialize into the welcome frame shape:
    /// {"ema":[7,14,30,60,200],
    ///  "macd":{"fast":12,"slow":26,"signal":9},
    ///  "supertrend":{"atr_period":10,"multiplier":3.0}}
    [[nodiscard]] QJsonObject toJson() const
    {
        QJsonArray ema;
        for (int period : emaPeriods)
        {
            ema.append(period);
        }

        QJsonObject macd;
        macd[QStringLiteral("fast")] = macdFast;
        macd[QStringLiteral("slow")] = macdSlow;
        macd[QStringLiteral("signal")] = macdSignal;

        QJsonObject supertrend;
        supertrend[QStringLiteral("atr_period")] = atrPeriod;
        supertrend[QStringLiteral("multiplier")] = supertrendMultiplier;

        QJsonObject params;
        params[QStringLiteral("ema")] = ema;
        params[QStringLiteral("macd")] = macd;
        params[QStringLiteral("supertrend")] = supertrend;
        return params;
    }
};

/// Every knob of the local data server in one value type.
///
/// The defaults are the deployed configuration: loopback-only listener on
/// 8090, engine control socket on 8081, XAUUSD CFD 1-minute candles.
struct DataServerConfig
{
    /// Loopback only. The protocol has no authentication, so binding a public
    /// interface would hand the engine's market data to the whole LAN.
    QString bindAddress = QStringLiteral("127.0.0.1");
    quint16 port = 8090;

    /// The engine's JSON-RPC control socket, polled over plain TCP. It is the
    /// same endpoint EngineController uses; no TLS and no token are involved.
    QString engineHost = QStringLiteral("127.0.0.1");
    quint16 enginePort = 8081;

    QString symbol = QStringLiteral("XAUUSD");
    QString marketType = QStringLiteral("cfd");
    QString timeframe = QStringLiteral("1m");

    /// History requested from get_market. 500 one-minute candles cover more
    /// than the longest EMA (200 bars) with room for the signal warm-up.
    int klineCount = 500;

    /// Engine poll cadence. The protocol caps broadcasts at 2 Hz, so 500 ms is
    /// both the poll interval and the update rate.
    int pollIntervalMs = 500;

    /// Concurrent consumers. Each one receives the same full dataset; the
    /// server does no per-client filtering or subscriptions.
    int maxClients = 8;

    /// The consumer pings every 5 s; the server drops a session that has been
    /// silent for 15 s, which is three missed pings.
    int clientPingIntervalMs = 5000;
    int clientIdleTimeoutMs = 15000;

    /// A session whose socket cannot drain 256 KiB is not reading; dropping it
    /// is cheaper than letting the write buffer grow without bound.
    int maxBackpressureBytes = 256 * 1024;

    /// Start the engine poller together with the listener. Tests that exercise
    /// only the network layer set this to false so no background connection to
    /// a live engine can race their assertions.
    bool pollEngine = true;

    IndicatorParams indicators;
};

} // namespace pulse::client::data
