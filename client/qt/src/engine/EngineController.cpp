#include "engine/EngineController.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QSettings>
#include <QVariantMap>

#include "models/AccountModel.hpp"
#include "models/OrdersModel.hpp"
#include "models/PositionsModel.hpp"
#include "models/ReportsModel.hpp"
#include "models/SignalsModel.hpp"
#include "rpc/RpcMethods.hpp"

namespace pulse::client
{

namespace
{
// Poll cadence. The market views (positions, orders) drive the dashboard's
// per-market tabs, which the user asked to refresh at 2 Hz. Balances stay at
// 0.2 Hz: get_account holds the engine's shared REST mutex across up to three
// signed exchange calls (plus optional ticker lookups), and that is the same
// mutex order placement waits on. Frequency is the only lever here — the hold
// time per call does not shrink — so 5 s keeps the dashboard current without
// turning every fill into a REST round-trip. Cheap in-memory methods keep
// their own cadence; signals are slower because nothing on screen needs them
// faster.
constexpr int kStatusIntervalMs = 2000;
constexpr int kPositionsIntervalMs = 500;
constexpr int kOrdersIntervalMs = 500;
constexpr int kSignalsIntervalMs = 10000;
constexpr int kAccountIntervalMs = 5000;

// Per-call budgets. The account budget is large because the engine performs up
// to three serialised REST calls behind a global mutex.
constexpr int kStatusTimeoutMs = 8000;
constexpr int kPositionsTimeoutMs = 15000;
constexpr int kOrdersTimeoutMs = 30000;
constexpr int kSignalsTimeoutMs = 10000;
constexpr int kAccountTimeoutMs = 120000;
constexpr int kMarketTimeoutMs = 15000;
} // namespace

EngineController::EngineController(QObject* parent)
    : QObject(parent)
    , m_fast(new RpcClient(QStringLiteral("fast"), this))
    , m_rest(new RpcClient(QStringLiteral("rest"), this))
    , m_slow(new RpcClient(QStringLiteral("slow"), this))
    , m_positionsModel(new PositionsModel(this))
    , m_ordersModel(new OrdersModel(this))
    , m_reportsModel(new ReportsModel(this))
    , m_signalsModel(new SignalsModel(this))
    , m_accountModel(new AccountModel(this))
    , m_marketStream(new MarketStream(this))
    , m_spotPositions(new MarketFilterModel(this))
    , m_cfdPositions(new MarketFilterModel(this))
    , m_futuresPositions(new MarketFilterModel(this))
    , m_spotOrders(new MarketFilterModel(this))
    , m_cfdOrders(new MarketFilterModel(this))
    , m_futuresOrders(new MarketFilterModel(this))
    , m_spotReports(new MarketFilterModel(this))
    , m_cfdReports(new MarketFilterModel(this))
    , m_futuresReports(new MarketFilterModel(this))
{
    // 1. Every socket feeds the same router; the request id decides which poll
    //    the reply belongs to. Each connection numbers from 1, so give them
    //    disjoint ranges or the replies land on the wrong poll. The ranges are
    //    wide (1e9) because the market polls run at 2 Hz: a 1e6 range — the
    //    original spacing — would be exhausted in about three days, after
    //    which the rest socket's ids would collide with the slow socket's.
    m_fast->setIdBase(1);
    m_rest->setIdBase(1'000'000'000);
    m_slow->setIdBase(2'000'000'000);

    // 1a. Per-market views. One source model per kind is polled once; each
    //     proxy narrows that snapshot to a single market_type. Every proxy
    //     gets an explicit market string — an empty filter would show every
    //     row on every tab.
    const auto bindMarket = [](MarketFilterModel* filter, QAbstractItemModel* source,
                               const QString& market)
    {
        filter->setSourceModel(source);
        filter->setMarketType(market);
    };
    bindMarket(m_spotPositions, m_positionsModel, QStringLiteral("spot"));
    bindMarket(m_cfdPositions, m_positionsModel, QStringLiteral("cfd"));
    bindMarket(m_futuresPositions, m_positionsModel, QStringLiteral("futures"));
    bindMarket(m_spotOrders, m_ordersModel, QStringLiteral("spot"));
    bindMarket(m_cfdOrders, m_ordersModel, QStringLiteral("cfd"));
    bindMarket(m_futuresOrders, m_ordersModel, QStringLiteral("futures"));
    bindMarket(m_spotReports, m_reportsModel, QStringLiteral("spot"));
    bindMarket(m_cfdReports, m_reportsModel, QStringLiteral("cfd"));
    bindMarket(m_futuresReports, m_reportsModel, QStringLiteral("futures"));

    for (RpcClient* client : { m_fast, m_rest, m_slow })
    {
        connect(client, &RpcClient::callSucceeded, this, &EngineController::handleSuccess);
        connect(client, &RpcClient::callFailed, this,
                [this](qint64 id, const RpcError& error)
        {
            m_inFlight.remove(id);
            handleFailure(error);
        });
        connect(client, &RpcClient::stateChanged, this,
                &EngineController::onConnectionStateChanged);
        connect(client, &RpcClient::authenticationFailed, this,
                [this](const QString& message)
        {
            setLastError(QStringLiteral("auth: ") + message);
            emit errorOccurred(m_lastError);
        });
    }

    // 1b. Each socket polls its own methods as soon as IT is ready. Calling
    //     refreshAll() when only the fast socket is up would fire requests at
    //     sockets that are still connecting, and those fail immediately.
    connect(m_fast, &RpcClient::stateChanged, this, [this](RpcClient::State state)
    {
        if (RpcClient::State::Ready == state && m_pollingEnabled)
        {
            pollStatus();
            pollSignals();
        }
    });
    connect(m_rest, &RpcClient::stateChanged, this, [this](RpcClient::State state)
    {
        if (RpcClient::State::Ready == state && m_pollingEnabled)
        {
            pollPositions();
            pollOrders();
        }
    });
    connect(m_slow, &RpcClient::stateChanged, this, [this](RpcClient::State state)
    {
        if (RpcClient::State::Ready == state && m_pollingEnabled)
        {
            pollAccount();
        }
    });

    // 2. One timer per method. A timer fires even while a request of the same
    //    kind is in flight; the poll functions refuse to stack.
    m_statusTimer.setInterval(kStatusIntervalMs);
    m_positionsTimer.setInterval(kPositionsIntervalMs);
    m_ordersTimer.setInterval(kOrdersIntervalMs);
    m_signalsTimer.setInterval(kSignalsIntervalMs);
    m_accountTimer.setInterval(kAccountIntervalMs);

    connect(&m_statusTimer, &QTimer::timeout, this, &EngineController::pollStatus);
    connect(&m_positionsTimer, &QTimer::timeout, this, &EngineController::pollPositions);
    connect(&m_ordersTimer, &QTimer::timeout, this, &EngineController::pollOrders);
    connect(&m_signalsTimer, &QTimer::timeout, this, &EngineController::pollSignals);
    connect(&m_accountTimer, &QTimer::timeout, this, &EngineController::pollAccount);

    // 2b. Dashboard preference, persisted across sessions under
    //     [dashboard] spotHoldings. Read here so the very first account poll
    //     already uses the saved choice.
    QSettings settings;
    m_spotHoldingsEnabled = settings
                                .value(QStringLiteral("dashboard/spotHoldings"), true)
                                .toBool();

    // 3. Optional headless auto-connect. PULSE_CLIENT_HOST turns the dialog off
    //    and connects at startup, which is what the smoke test uses.
    //    PULSE_CLIENT_PORT defaults to 8081, PULSE_CLIENT_TLS enables TLS and
    //    PULSE_CLIENT_TOKEN supplies the token.
    const QString autoHost = qEnvironmentVariable("PULSE_CLIENT_HOST");
    if (!autoHost.isEmpty())
    {
        m_autoConnect = true;
        const int autoPort = qEnvironmentVariableIntValue("PULSE_CLIENT_PORT");
        const bool autoTls = qEnvironmentVariableIsSet("PULSE_CLIENT_TLS");
        const QString autoToken = qEnvironmentVariable("PULSE_CLIENT_TOKEN");

        // Queued so the whole object graph exists before the first signal.
        QTimer::singleShot(0, this, [this, autoHost, autoPort, autoTls, autoToken]()
        {
            connectToEngine(autoHost, autoPort > 0 ? autoPort : 8081,
                            autoTls, autoToken);
        });
    }
}

void EngineController::connectToEngine(const QString& host, int port, bool tls,
                                       const QString& token)
{
    // 1. All three sockets share one endpoint and one token.
    for (RpcClient* client : { m_fast, m_rest, m_slow })
    {
        client->setEndpoint(host, static_cast<quint16>(port), tls);
        client->setToken(token);
    }

    // 1a. The market stream keeps the same endpoint but opens its socket
    //     lazily, only when the Market page starts streaming.
    m_marketStream->setEndpoint(host, static_cast<quint16>(port), tls, token);

    // 2. The fast socket drives the aggregate state; the other two follow.
    m_fast->connectToEngine();
    m_rest->connectToEngine();
    m_slow->connectToEngine();
}

void EngineController::disconnectFromEngine()
{
    stopPolling();
    m_marketStream->stop();
    for (RpcClient* client : { m_fast, m_rest, m_slow })
    {
        client->disconnectFromEngine();
    }
    m_inFlight.clear();
}

void EngineController::setPollingEnabled(bool enabled)
{
    if (enabled == m_pollingEnabled)
    {
        return;
    }
    m_pollingEnabled = enabled;
    emit pollingEnabledChanged();

    if (enabled)
    {
        if (ConnectionState::Ready == m_connectionState)
        {
            startPolling();
        }
    }
    else
    {
        stopPolling();
        // The live chart is a poller too; leaving it running while the user
        // asked the client to stop polling would contradict the setting.
        m_marketStream->stop();
    }
}

void EngineController::setSpotHoldingsEnabled(bool enabled)
{
    if (enabled == m_spotHoldingsEnabled)
    {
        return;
    }
    m_spotHoldingsEnabled = enabled;
    QSettings().setValue(QStringLiteral("dashboard/spotHoldings"), enabled);
    emit spotHoldingsEnabledChanged();

    // Ask for a fresh snapshot right away so the table appears or clears
    // without waiting up to 30 s for the next account poll. pollAccount()
    // refuses to stack a second request while one is in flight.
    if (RpcClient::State::Ready == m_slow->state())
    {
        pollAccount();
    }
}

void EngineController::refreshAll()
{
    pollStatus();
    pollPositions();
    pollOrders();
    pollSignals();
    pollAccount();
}

void EngineController::startPolling()
{
    m_statusTimer.start();
    m_positionsTimer.start();
    m_ordersTimer.start();
    m_signalsTimer.start();
    m_accountTimer.start();

    // First paint should not wait for the first tick.
    refreshAll();
}

void EngineController::stopPolling()
{
    m_statusTimer.stop();
    m_positionsTimer.stop();
    m_ordersTimer.stop();
    m_signalsTimer.stop();
    m_accountTimer.stop();
}

bool EngineController::isInFlight(PollKind kind) const
{
    for (PollKind value : m_inFlight)
    {
        if (value == kind)
        {
            return true;
        }
    }
    return false;
}

void EngineController::pollStatus()
{
    if (RpcClient::State::Ready != m_fast->state() || isInFlight(PollKind::Status))
    {
        return;
    }
    const qint64 id = m_fast->call(methods::kGetStatus, QJsonObject{}, kStatusTimeoutMs);
    m_inFlight.insert(id, PollKind::Status);
}

void EngineController::pollPositions()
{
    if (RpcClient::State::Ready != m_rest->state() || isInFlight(PollKind::Positions))
    {
        return;
    }
    const qint64 id = m_rest->call(methods::kGetPositions, QJsonObject{}, kPositionsTimeoutMs);
    m_inFlight.insert(id, PollKind::Positions);
}

void EngineController::pollOrders()
{
    if (RpcClient::State::Ready != m_rest->state() || isInFlight(PollKind::Orders))
    {
        return;
    }
    const qint64 id = m_rest->call(methods::kGetOrders, QJsonObject{}, kOrdersTimeoutMs);
    m_inFlight.insert(id, PollKind::Orders);
}

void EngineController::pollSignals()
{
    if (RpcClient::State::Ready != m_fast->state() || isInFlight(PollKind::Signals))
    {
        return;
    }
    const qint64 id = m_fast->call(methods::kGetSignals, QJsonObject{}, kSignalsTimeoutMs);
    m_inFlight.insert(id, PollKind::Signals);
}

void EngineController::pollAccount()
{
    if (RpcClient::State::Ready != m_slow->state() || isInFlight(PollKind::Account))
    {
        return;
    }
    // The engine only builds the per-currency breakdown when asked, because
    // that path can issue public REST ticker calls while holding the shared
    // REST mutex. The dashboard switch decides whether to pay for it.
    QJsonObject params;
    params[QStringLiteral("include_spot_balances")] = m_spotHoldingsEnabled;

    const qint64 id = m_slow->call(methods::kGetAccount, params, kAccountTimeoutMs);
    m_inFlight.insert(id, PollKind::Account);
}

void EngineController::fetchMarket(const QString& symbol)
{
    if (RpcClient::State::Ready != m_rest->state() || symbol.isEmpty())
    {
        return;
    }

    // The engine reads named parameters for this method only.
    QJsonObject params;
    params[QStringLiteral("symbol")] = symbol;
    params[QStringLiteral("book_levels")] = 5;

    const qint64 id = m_rest->call(methods::kGetMarket, params, kMarketTimeoutMs);
    m_inFlight.insert(id, PollKind::Market);
}

void EngineController::handleSuccess(qint64 id, const QJsonObject& result)
{
    auto it = m_inFlight.find(id);
    if (it == m_inFlight.end())
    {
        return;
    }
    const PollKind kind = it.value();
    m_inFlight.erase(it);

    switch (kind)
    {
    case PollKind::Status:
    {
        // 1. Scalars first; every one of these is always present.
        m_engineVersion = result.value(QStringLiteral("version")).toString();
        m_engineUptimeSec = result.value(QStringLiteral("uptime_sec")).toDouble();
        m_network = result.value(QStringLiteral("network")).toString();
        m_tradingHalted = result.value(QStringLiteral("trading_halted")).toBool();
        m_activeMarket = result.value(QStringLiteral("active_market")).toString();
        m_openPositions = result.value(QStringLiteral("open_positions")).toInt();
        m_strategiesRunning = result.value(QStringLiteral("strategies_running")).toInt();
        m_strategiesTotal = result.value(QStringLiteral("strategies_total")).toInt();

        // 2. Symbol list, de-duplicated and sorted for a stable picker.
        QStringList list;
        const QJsonArray symbols = result.value(QStringLiteral("symbols")).toArray();
        for (const QJsonValue& value : symbols)
        {
            const QString symbol = value.toString();
            if (!symbol.isEmpty() && !list.contains(symbol))
            {
                list << symbol;
            }
        }
        list.sort();
        m_symbols = list;

        setLastError(QString());
        emit statusChanged();
        break;
    }
    case PollKind::Positions:
        m_positionsModel->setRows(result.value(QStringLiteral("positions")).toArray());
        setLastError(QString());
        break;
    case PollKind::Orders:
        m_ordersModel->setRows(result.value(QStringLiteral("activeOrders")).toArray());
        m_reportsModel->setRows(result.value(QStringLiteral("recentReports")).toArray());
        setLastError(QString());
        break;
    case PollKind::Signals:
        m_signalsModel->setRows(result.value(QStringLiteral("signals")).toArray());
        setLastError(QString());
        break;
    case PollKind::Account:
        m_accountModel->setFromJson(result);
        setLastError(QString());
        break;
    case PollKind::Market:
        // In-band logical failures arrive inside a successful result.
        if (result.contains(QStringLiteral("error")))
        {
            setLastError(result.value(QStringLiteral("error")).toString());
            break;
        }
        emit marketReceived(result.toVariantMap());
        setLastError(QString());
        break;
    }
}

void EngineController::handleFailure(const RpcError& error)
{
    // The last known data stays on screen; pages render it with a stale badge.
    setLastError(error.toString());
    emit errorOccurred(error.toString());
}

void EngineController::onConnectionStateChanged()
{
    ConnectionState next = ConnectionState::Disconnected;
    switch (m_fast->state())
    {
    case RpcClient::State::Connecting:
    case RpcClient::State::Authenticating:
        next = ConnectionState::Connecting;
        break;
    case RpcClient::State::Ready:
        next = ConnectionState::Ready;
        break;
    case RpcClient::State::Disconnected:
        next = ConnectionState::Disconnected;
        break;
    }

    if (next == m_connectionState)
    {
        return;
    }
    m_connectionState = next;
    emit connectionStateChanged();

    if (ConnectionState::Ready == next && m_pollingEnabled)
    {
        startPolling();
    }
    else if (ConnectionState::Disconnected == next)
    {
        stopPolling();
    }
}

void EngineController::setLastError(const QString& message)
{
    if (message == m_lastError)
    {
        return;
    }
    m_lastError = message;
    emit lastErrorChanged();
}

} // namespace pulse::client
