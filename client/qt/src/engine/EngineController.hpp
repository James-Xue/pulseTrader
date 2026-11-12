#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "rpc/RpcClient.hpp"

// The models are Q_PROPERTY pointer types, so moc needs their complete
// definitions: a forward declaration trips the "Pointer Meta Types must either
// point to fully-defined types" static assertion.
#include "market/MarketStream.hpp"
#include "models/AccountModel.hpp"
#include "models/MarketFilterModel.hpp"
#include "models/OrdersModel.hpp"
#include "models/PositionsModel.hpp"
#include "models/ReportsModel.hpp"
#include "models/RiskModel.hpp"
#include "models/SignalsModel.hpp"
#include "models/StrategiesModel.hpp"
#include "models/StrategyParamsModel.hpp"
#include "models/SymbolFilterModel.hpp"

namespace pulse::client
{

/// Facade between the QML layer and the engine's read-only control methods.
///
/// Responsibilities:
/// 1. Own three RpcClient connections, because the engine dispatches one
///    request at a time per connection and serialises REST calls behind a
///    global mutex. Separate sockets keep a slow `get_account` from delaying
///    `get_status`.
/// 2. Poll the read-only methods on independent timers, never stacking a
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

    /// Read-only gold-trader surface. `strategiesModel` is the engine's full
    /// instance list; `goldStrategies` narrows it to symbol XAUUSD. The
    /// parameters model belongs to the currently selected instance and is
    /// fetched on demand, not polled.
    Q_PROPERTY(pulse::client::StrategiesModel* strategiesModel READ strategiesModel CONSTANT)
    Q_PROPERTY(pulse::client::SymbolFilterModel* goldStrategies READ goldStrategies CONSTANT)
    Q_PROPERTY(pulse::client::RiskModel* riskModel READ riskModel CONSTANT)
    Q_PROPERTY(pulse::client::StrategyParamsModel* strategyParamsModel READ strategyParamsModel CONSTANT)
    Q_PROPERTY(QString selectedStrategyId READ selectedStrategyId NOTIFY selectedStrategyChanged)
    Q_PROPERTY(QVariantMap selectedStrategy READ selectedStrategy NOTIFY selectedStrategyChanged)

    /// One-symbol 1 Hz price stream used by the Market page. The stream owns
    /// its own control socket and stays idle until the page calls start(), so
    /// the dashboard pays nothing for it.
    Q_PROPERTY(pulse::client::MarketStream* marketStream READ marketStream CONSTANT)

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

    /// Select a gold instance and fetch its parameters on demand. An empty id
    /// clears the selection. Calling it again with the same id re-fetches,
    /// which is what the page's Retry button relies on.
    Q_INVOKABLE void selectStrategy(const QString& strategyId);

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
    [[nodiscard]] StrategiesModel* strategiesModel() const { return m_strategiesModel; }
    [[nodiscard]] SymbolFilterModel* goldStrategies() const { return m_goldStrategies; }
    [[nodiscard]] RiskModel* riskModel() const { return m_riskModel; }
    [[nodiscard]] StrategyParamsModel* strategyParamsModel() const
    {
        return m_strategyParamsModel;
    }
    [[nodiscard]] QString selectedStrategyId() const { return m_selectedStrategyId; }

    /// The selected row as a plain map (empty when nothing is selected), so
    /// QML does not have to look a row up by index.
    [[nodiscard]] QVariantMap selectedStrategy() const;
    [[nodiscard]] MarketStream* marketStream() const { return m_marketStream; }

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
    /// Fires on select, on selection loss, and on every strategies refresh.
    void selectedStrategyChanged();
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
        Strategies,
        Risk,
        StrategyParams,
    };

    void startPolling();
    void stopPolling();
    void pollStatus();
    void pollPositions();
    void pollOrders();
    void pollSignals();
    void pollAccount();
    void pollStrategies();
    void pollRisk();
    void issueStrategyParams(const QString& strategyId);
    void handleSuccess(qint64 id, const QJsonObject& result);
    void handleArraySuccess(qint64 id, const QJsonArray& result);
    void handleParamsFailure(const QString& strategyId, const RpcError& error);
    void maybeReissueParams();
    void clearStrategySelection();
    void handleFailure(const RpcError& error);
    void onConnectionStateChanged();
    void setLastError(const QString& message);
    void clearInFlight(PollKind kind);

    [[nodiscard]] bool isInFlight(PollKind kind) const;

    RpcClient* m_fast = nullptr; ///< get_status, get_signals, list_strategies, get_risk
    RpcClient* m_rest = nullptr; ///< get_positions, get_orders
    RpcClient* m_slow = nullptr; ///< get_account

    QTimer m_statusTimer;
    QTimer m_positionsTimer;
    QTimer m_ordersTimer;
    QTimer m_signalsTimer;
    QTimer m_accountTimer;
    QTimer m_strategiesTimer;
    QTimer m_riskTimer;

    QHash<qint64, PollKind> m_inFlight;
    /// Request id -> strategy id for in-flight get_strategy_params calls. The
    /// reply never echoes the id, so this is the only way to attribute it.
    QHash<qint64, QString> m_paramsRequests;

    PositionsModel* m_positionsModel = nullptr;
    OrdersModel* m_ordersModel = nullptr;
    ReportsModel* m_reportsModel = nullptr;
    SignalsModel* m_signalsModel = nullptr;
    AccountModel* m_accountModel = nullptr;
    MarketStream* m_marketStream = nullptr;
    StrategiesModel* m_strategiesModel = nullptr;
    SymbolFilterModel* m_goldStrategies = nullptr;
    RiskModel* m_riskModel = nullptr;
    StrategyParamsModel* m_strategyParamsModel = nullptr;

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
    /// Last `list_strategies` payload, so selectedStrategy() can return the
    /// row without a model-row lookup.
    QJsonArray m_strategiesRaw;
    QString m_selectedStrategyId;
    QString m_lastError;
    bool m_pollingEnabled = true;
    bool m_spotHoldingsEnabled = true;
    bool m_autoConnect = false;
};

} // namespace pulse::client
