#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include "rpc/RpcClient.hpp"

// The models are Q_PROPERTY pointer types, so moc needs their complete
// definitions: a forward declaration trips the "Pointer Meta Types must either
// point to fully-defined types" static assertion.
#include "models/AccountModel.hpp"
#include "models/MarketFilterModel.hpp"
#include "models/OrdersModel.hpp"
#include "models/PositionsModel.hpp"
#include "models/ReportsModel.hpp"
#include "models/SignalsModel.hpp"

namespace pulse::client
{

/// Facade between the QML layer and the engine's read-only control methods.
///
/// Responsibilities:
/// 1. Own three RpcClient connections, because the engine dispatches one
///    request at a time per connection and serialises REST calls behind a
///    global mutex. Separate sockets keep a slow `get_account` from delaying
///    `get_status`.
/// 2. Poll the six read-only methods on independent timers, never stacking a
///    second request while one of that kind is still in flight.
/// 3. Map wire payloads onto models and scalar properties, preserving the
///    distinction between "absent" and "zero".
///
/// Thread affinity: GUI thread only.
class EngineController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(ConnectionState connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(QString engineVersion READ engineVersion NOTIFY statusChanged)
    Q_PROPERTY(double engineUptimeSec READ engineUptimeSec NOTIFY statusChanged)
    Q_PROPERTY(QString network READ network NOTIFY statusChanged)
    Q_PROPERTY(bool tradingHalted READ tradingHalted NOTIFY statusChanged)
    Q_PROPERTY(QString activeMarket READ activeMarket NOTIFY statusChanged)
    Q_PROPERTY(QStringList symbols READ symbols NOTIFY statusChanged)
    Q_PROPERTY(int openPositions READ openPositions NOTIFY statusChanged)
    Q_PROPERTY(int strategiesRunning READ strategiesRunning NOTIFY statusChanged)
    Q_PROPERTY(int strategiesTotal READ strategiesTotal NOTIFY statusChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool pollingEnabled READ pollingEnabled NOTIFY pollingEnabledChanged)
    Q_PROPERTY(bool spotHoldingsEnabled READ spotHoldingsEnabled
               NOTIFY spotHoldingsEnabledChanged)
    Q_PROPERTY(bool autoConnect READ autoConnect CONSTANT)

    Q_PROPERTY(pulse::client::PositionsModel* positionsModel READ positionsModel CONSTANT)
    Q_PROPERTY(pulse::client::OrdersModel* ordersModel READ ordersModel CONSTANT)
    Q_PROPERTY(pulse::client::ReportsModel* reportsModel READ reportsModel CONSTANT)
    Q_PROPERTY(pulse::client::SignalsModel* signalsModel READ signalsModel CONSTANT)
    Q_PROPERTY(pulse::client::AccountModel* accountModel READ accountModel CONSTANT)

    // Per-market views over the three list models. The source models stay the
    // single polled snapshot; these proxies only narrow it, so switching a
    // dashboard tab costs no request. Empty marketType = all rows.
    Q_PROPERTY(pulse::client::MarketFilterModel* spotPositions READ spotPositions CONSTANT)
    Q_PROPERTY(pulse::client::MarketFilterModel* cfdPositions READ cfdPositions CONSTANT)
    Q_PROPERTY(pulse::client::MarketFilterModel* futuresPositions READ futuresPositions CONSTANT)
    Q_PROPERTY(pulse::client::MarketFilterModel* spotOrders READ spotOrders CONSTANT)
    Q_PROPERTY(pulse::client::MarketFilterModel* cfdOrders READ cfdOrders CONSTANT)
    Q_PROPERTY(pulse::client::MarketFilterModel* futuresOrders READ futuresOrders CONSTANT)
    Q_PROPERTY(pulse::client::MarketFilterModel* spotReports READ spotReports CONSTANT)
    Q_PROPERTY(pulse::client::MarketFilterModel* cfdReports READ cfdReports CONSTANT)
    Q_PROPERTY(pulse::client::MarketFilterModel* futuresReports READ futuresReports CONSTANT)

public:
    /// Connection state aggregated from the three sockets. The UI only needs
    /// "can I read data or not", so the first ready socket is not enough: the
    /// state follows the fast socket and errors are surfaced separately.
    enum class ConnectionState
    {
        Disconnected,
        Connecting,
        Ready,
    };
    Q_ENUM(ConnectionState)

    explicit EngineController(QObject* parent = nullptr);

    Q_INVOKABLE void connectToEngine(const QString& host, int port, bool tls,
                                     const QString& token);
    Q_INVOKABLE void disconnectFromEngine();
    Q_INVOKABLE void refreshAll();
    Q_INVOKABLE void setPollingEnabled(bool enabled);
    Q_INVOKABLE void fetchMarket(const QString& symbol);

    /// Request the per-currency spot breakdown from get_account. Disabled by
    /// default: pricing every unpriced coin costs public REST ticker calls on
    /// the engine's shared REST mutex, which delays the account snapshot and
    /// every other REST consumer. The setting persists in the UI layer.
    Q_INVOKABLE void setSpotHoldingsEnabled(bool enabled);

    [[nodiscard]] ConnectionState connectionState() const { return m_connectionState; }
    [[nodiscard]] QString engineVersion() const { return m_engineVersion; }
    [[nodiscard]] double engineUptimeSec() const { return m_engineUptimeSec; }
    [[nodiscard]] QString network() const { return m_network; }
    [[nodiscard]] bool tradingHalted() const { return m_tradingHalted; }
    [[nodiscard]] QString activeMarket() const { return m_activeMarket; }
    [[nodiscard]] QStringList symbols() const { return m_symbols; }
    [[nodiscard]] int openPositions() const { return m_openPositions; }
    [[nodiscard]] int strategiesRunning() const { return m_strategiesRunning; }
    [[nodiscard]] int strategiesTotal() const { return m_strategiesTotal; }
    [[nodiscard]] QString lastError() const { return m_lastError; }
    [[nodiscard]] bool pollingEnabled() const { return m_pollingEnabled; }
    [[nodiscard]] bool spotHoldingsEnabled() const { return m_spotHoldingsEnabled; }

    /// True when PULSE_CLIENT_HOST was set at startup, in which case the
    /// controller connected without waiting for the dialog. Used by smoke
    /// tests and headless runs.
    [[nodiscard]] bool autoConnect() const { return m_autoConnect; }

    [[nodiscard]] PositionsModel* positionsModel() const { return m_positionsModel; }
    [[nodiscard]] OrdersModel* ordersModel() const { return m_ordersModel; }
    [[nodiscard]] ReportsModel* reportsModel() const { return m_reportsModel; }
    [[nodiscard]] SignalsModel* signalsModel() const { return m_signalsModel; }
    [[nodiscard]] AccountModel* accountModel() const { return m_accountModel; }

    [[nodiscard]] MarketFilterModel* spotPositions() const { return m_spotPositions; }
    [[nodiscard]] MarketFilterModel* cfdPositions() const { return m_cfdPositions; }
    [[nodiscard]] MarketFilterModel* futuresPositions() const { return m_futuresPositions; }
    [[nodiscard]] MarketFilterModel* spotOrders() const { return m_spotOrders; }
    [[nodiscard]] MarketFilterModel* cfdOrders() const { return m_cfdOrders; }
    [[nodiscard]] MarketFilterModel* futuresOrders() const { return m_futuresOrders; }
    [[nodiscard]] MarketFilterModel* spotReports() const { return m_spotReports; }
    [[nodiscard]] MarketFilterModel* cfdReports() const { return m_cfdReports; }
    [[nodiscard]] MarketFilterModel* futuresReports() const { return m_futuresReports; }

signals:
    void connectionStateChanged();
    void statusChanged();
    void lastErrorChanged();
    void pollingEnabledChanged();
    void spotHoldingsEnabledChanged();
    void errorOccurred(const QString& message);
    void marketReceived(const QVariantMap& market);

private:
    /// Which poll a response belongs to. Stored per request id so a reply can
    /// be routed without keeping a separate handler per socket.
    enum class PollKind
    {
        Status,
        Positions,
        Orders,
        Signals,
        Account,
        Market,
    };

    void startPolling();
    void stopPolling();
    void pollStatus();
    void pollPositions();
    void pollOrders();
    void pollSignals();
    void pollAccount();
    void handleSuccess(qint64 id, const QJsonObject& result);
    void handleFailure(const RpcError& error);
    void onConnectionStateChanged();
    void setLastError(const QString& message);
    void clearInFlight(PollKind kind);

    [[nodiscard]] bool isInFlight(PollKind kind) const;

    RpcClient* m_fast = nullptr; ///< get_status, get_signals
    RpcClient* m_rest = nullptr; ///< get_positions, get_orders
    RpcClient* m_slow = nullptr; ///< get_account

    QTimer m_statusTimer;
    QTimer m_positionsTimer;
    QTimer m_ordersTimer;
    QTimer m_signalsTimer;
    QTimer m_accountTimer;

    QHash<qint64, PollKind> m_inFlight;

    PositionsModel* m_positionsModel = nullptr;
    OrdersModel* m_ordersModel = nullptr;
    ReportsModel* m_reportsModel = nullptr;
    SignalsModel* m_signalsModel = nullptr;
    AccountModel* m_accountModel = nullptr;

    MarketFilterModel* m_spotPositions = nullptr;
    MarketFilterModel* m_cfdPositions = nullptr;
    MarketFilterModel* m_futuresPositions = nullptr;
    MarketFilterModel* m_spotOrders = nullptr;
    MarketFilterModel* m_cfdOrders = nullptr;
    MarketFilterModel* m_futuresOrders = nullptr;
    MarketFilterModel* m_spotReports = nullptr;
    MarketFilterModel* m_cfdReports = nullptr;
    MarketFilterModel* m_futuresReports = nullptr;

    ConnectionState m_connectionState = ConnectionState::Disconnected;
    QString m_engineVersion;
    double m_engineUptimeSec = 0.0;
    QString m_network;
    bool m_tradingHalted = false;
    QString m_activeMarket;
    QStringList m_symbols;
    int m_openPositions = 0;
    int m_strategiesRunning = 0;
    int m_strategiesTotal = 0;
    QString m_lastError;
    bool m_pollingEnabled = true;
    bool m_spotHoldingsEnabled = true;
    bool m_autoConnect = false;
};

} // namespace pulse::client
