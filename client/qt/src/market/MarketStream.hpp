#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "market/CandleAggregator.hpp"
#include "rpc/RpcClient.hpp"

namespace pulse::client
{

/// One-symbol live price stream for the Market page.
///
/// Responsibilities:
/// 1. Own a dedicated RpcClient against the engine's control socket. The
///    engine dispatches one request per connection, so this socket never adds
///    latency to the dashboard's status/position/order polls.
/// 2. Poll `get_market` at a fixed cadence (1 Hz by default) and never stack a
///    second request while one is in flight.
/// 3. Fold every fresh ticker into a CandleAggregator, turning the 1-second
///    samples into 10-second OHLC candles by default.
/// 4. Expose the newest ticker fields and the candle list as QML properties.
///
/// Freshness:
/// - Only a genuine ticker with a positive exchange timestamp may enter the
///   candle aggregator. A ticker with no timestamp cannot be proven fresh, and
///   the newest 1-minute kline close is a display-only fallback: repeating
///   either once per second would draw a flat candle that never traded.
/// - A ticker older than `kStaleAfterMs` is displayed but not folded.
/// - The stale flag also flips on after three missed poll intervals, which
///   covers an engine that is reachable but has stopped replying.
///
/// Thread affinity: GUI thread only. The RpcClient is asynchronous, so no
/// socket operation blocks QML.
class MarketStream : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString symbol READ symbol NOTIFY symbolChanged)
    Q_PROPERTY(QString marketType READ marketType NOTIFY marketTypeChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)
    Q_PROPERTY(bool engineReady READ engineReady NOTIFY engineReadyChanged)
    Q_PROPERTY(bool stale READ stale NOTIFY staleChanged)

    Q_PROPERTY(double lastPrice READ lastPrice NOTIFY tickerChanged)
    Q_PROPERTY(double bid READ bid NOTIFY tickerChanged)
    Q_PROPERTY(double ask READ ask NOTIFY tickerChanged)
    Q_PROPERTY(double changePct READ changePct NOTIFY tickerChanged)
    Q_PROPERTY(double volume24h READ volume24h NOTIFY tickerChanged)
    Q_PROPERTY(qint64 tickerTimestamp READ tickerTimestamp NOTIFY tickerChanged)
    Q_PROPERTY(QVariantMap rawTicker READ rawTicker NOTIFY tickerChanged)

    Q_PROPERTY(int sampleCount READ sampleCount NOTIFY sampleChanged)
    Q_PROPERTY(qint64 lastSampleMs READ lastSampleMs NOTIFY sampleChanged)
    Q_PROPERTY(int candlePeriodSec READ candlePeriodSec WRITE setCandlePeriodSec
                   NOTIFY candlePeriodSecChanged)
    Q_PROPERTY(QVariantList candles READ candles NOTIFY candlesChanged)

    Q_PROPERTY(int pollIntervalMs READ pollIntervalMs WRITE setPollIntervalMs
                   NOTIFY pollIntervalMsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

  public:
    explicit MarketStream(QObject* parent = nullptr);
    ~MarketStream() override;

    /// Configure the control socket without opening it. Streaming starts the
    /// connection lazily, so a client that never opens the Market page never
    /// pays for an extra socket.
    void setEndpoint(const QString& host, quint16 port, bool tls,
                     const QString& token);

    /// Begin streaming `symbol` (e.g. BTC_USDT). `marketType` is one of
    /// spot/futures/cfd; an empty value lets the engine pick its active
    /// market. Restarting with the same symbol/market is a cheap refresh.
    Q_INVOKABLE void start(const QString& symbol,
                           const QString& marketType = QString());

    /// Stop polling and drop the control socket. The last candle stays on
    /// screen; the next start() clears it.
    Q_INVOKABLE void stop();

    /// Drop collected candles without stopping the stream. Used by the period
    /// selector and by tests.
    Q_INVOKABLE void clearCandles();

    [[nodiscard]] QString symbol() const { return m_symbol; }
    [[nodiscard]] QString marketType() const { return m_marketType; }
    [[nodiscard]] bool streaming() const { return m_streaming; }
    [[nodiscard]] bool engineReady() const { return m_engineReady; }
    [[nodiscard]] bool stale() const { return m_stale; }

    [[nodiscard]] double lastPrice() const { return m_lastPrice; }
    [[nodiscard]] double bid() const { return m_bid; }
    [[nodiscard]] double ask() const { return m_ask; }
    [[nodiscard]] double changePct() const { return m_changePct; }
    [[nodiscard]] double volume24h() const { return m_volume24h; }
    [[nodiscard]] qint64 tickerTimestamp() const { return m_tickerTimestamp; }
    [[nodiscard]] QVariantMap rawTicker() const { return m_rawTicker; }

    [[nodiscard]] int sampleCount() const { return m_sampleCount; }
    [[nodiscard]] qint64 lastSampleMs() const { return m_lastSampleMs; }
    [[nodiscard]] int candlePeriodSec() const { return m_candlePeriodSec; }
    [[nodiscard]] QVariantList candles() const { return m_candles; }

    [[nodiscard]] int pollIntervalMs() const { return m_pollIntervalMs; }
    [[nodiscard]] QString lastError() const { return m_lastError; }

    void setCandlePeriodSec(int seconds);
    void setPollIntervalMs(int milliseconds);

  signals:
    void symbolChanged();
    void marketTypeChanged();
    void streamingChanged();
    void engineReadyChanged();
    void staleChanged();
    void tickerChanged();
    void sampleChanged();
    void candlePeriodSecChanged();
    void candlesChanged();
    void pollIntervalMsChanged();
    void lastErrorChanged();

  private:
    /// Arm one get_market request unless one is already in flight.
    void poll();

    /// Route a reply. The id check discards a straggler from a stopped or
    /// restarted stream.
    void onCallSucceeded(qint64 id, const QJsonObject& result);
    void onCallFailed(qint64 id, const RpcError& error);

    /// Fold the current ticker into the candle list and republish the QML
    /// payload.
    void applyTicker(const QJsonObject& ticker, const QJsonArray& klines);

    /// Rebuild the QVariantList the QML Canvas consumes.
    void rebuildCandles();

    void refreshStale();
    void setEngineReady(bool ready);
    void setStale(bool stale);
    void setLastError(const QString& message);

    static constexpr int kCallTimeoutMs = 2000;
    static constexpr qint64 kStaleAfterMs = 5000;
    static constexpr int kDefaultPollIntervalMs = 1000;
    static constexpr int kDefaultCandlePeriodSec = 10;
    static constexpr int kMaxCandles = 120;
    static constexpr int kMinPollIntervalMs = 50;
    static constexpr int kMaxPollIntervalMs = 60'000;

    RpcClient* m_rpc = nullptr;
    QTimer m_pollTimer;

    QString m_symbol;
    QString m_marketType;
    bool m_streaming = false;
    bool m_engineReady = false;
    bool m_stale = true;
    bool m_inFlight = false;
    qint64 m_inFlightId = -1;

    int m_pollIntervalMs = kDefaultPollIntervalMs;
    int m_candlePeriodSec = kDefaultCandlePeriodSec;

    double m_lastPrice = 0.0;
    double m_bid = 0.0;
    double m_ask = 0.0;
    double m_changePct = 0.0;
    double m_volume24h = 0.0;
    qint64 m_tickerTimestamp = 0;
    QVariantMap m_rawTicker;

    int m_sampleCount = 0;
    qint64 m_lastSampleMs = 0;
    QVariantList m_candles;

    QString m_lastError;
    CandleAggregator m_aggregator;
};

} // namespace pulse::client