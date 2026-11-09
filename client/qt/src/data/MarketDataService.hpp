#pragma once

#include <QJsonObject>
#include <QObject>
#include <QTimer>

#include "data/DataServerConfig.hpp"
#include "rpc/RpcClient.hpp"

namespace pulse::client::data
{

/// Polls the engine's get_market method and turns the replies into the two
/// payloads the data server publishes.
///
/// Responsibilities:
/// 1. Own one dedicated RpcClient ("data-server") against the loopback control
///    socket, with no TLS and no token.
/// 2. Poll get_market every `pollIntervalMs`, never stacking a second request
///    while one is in flight (the engine dispatches one request per
///    connection, so a stacked call would only build a backlog).
/// 3. Compute indicators from the returned closed klines and emit:
///    - snapshotReady once per cold start, on every recovery, and periodically
///      so a late-joining consumer still gets a fresh 500-candle baseline;
///    - updateReady on every poll, carrying the ticker, the newest kline and
///      the indicators.
///
/// The GUI thread never blocks: every socket operation is asynchronous and the
/// poll only writes when the transport is Ready.
class MarketDataService : public QObject
{
    Q_OBJECT

public:
    explicit MarketDataService(const DataServerConfig& config,
                               QObject* parent = nullptr);
    ~MarketDataService() override;

    /// Connect the control socket and start the poll timer.
    void start();

    /// Stop polling and close the control socket.
    void stop();

    [[nodiscard]] bool engineReady() const
    {
        return m_engineReady;
    }

    /// Most recent full snapshot, empty until the first successful poll.
    [[nodiscard]] QJsonObject latestSnapshot() const
    {
        return m_latestSnapshot;
    }

signals:
    /// Full params object for a "snapshot" notification.
    void snapshotReady(const QJsonObject& params);

    /// Full params object for a "market.update" notification.
    void updateReady(const QJsonObject& params);

    void engineReadyChanged(bool ready);

private:
    /// Send one get_market request unless a previous one is still unanswered.
    void poll();

    /// Reply handlers. The id check rejects a straggler from an earlier poll.
    void onCallSucceeded(qint64 id, const QJsonObject& result);
    void onCallFailed(qint64 id);

    void onMarketResult(const QJsonObject& result);
    void publishUpdate(bool forceStale);

    void setEngineReady(bool ready);

    /// True when the last ticker is older than the staleness budget, or when
    /// no data is available at all.
    [[nodiscard]] bool isStale() const;

    DataServerConfig m_config;
    RpcClient* m_rpc = nullptr;
    QTimer m_pollTimer;

    bool m_inFlight = false;
    qint64 m_inFlightId = -1;

    /// Producer-side sequence. DataServer re-stamps every broadcast with its
    /// own counter; this one keeps the emitted payloads ordered for direct
    /// consumers and for diagnostics.
    qint64 m_seq = 0;

    int m_successfulPolls = 0;
    bool m_engineReady = false;

    /// Timestamp of the last published update. The protocol caps the stream at
    /// 2 Hz, and an extra trigger (the socket turning Ready) can fire between
    /// two timer ticks, so the cadence is enforced here rather than assumed.
    qint64 m_lastUpdateMs = 0;

    /// Last known values, replayed while the engine is unreachable so a
    /// consumer sees a continuous series instead of a gap.
    QJsonObject m_lastTicker;
    QJsonObject m_lastKline;
    QJsonObject m_lastIndicators;

    QJsonObject m_latestSnapshot;
};

} // namespace pulse::client::data
