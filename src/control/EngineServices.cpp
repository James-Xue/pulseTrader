// EngineServices.cpp — see EngineServices.hpp

#include "control/EngineServices.hpp"

#include "control/OrderFlowExecutor.hpp"
#include "control/OrderIdempotency.hpp"
#include "grid/GridManager.hpp"
#include "ai/ParamBounds.hpp"
#include "core/snapshot_types.hpp"
#include "core/types.hpp"
#include "execution/ExecutionReport.hpp"
#include "execution/ExchangeStopPlan.hpp"
#include "execution/OrderExecutor.hpp"
#include "logging/Logger.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/GoldTrader.hpp"

#ifdef PULSE_ENABLE_SQLITE
#include "trade_recorder/TradeRecorder.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace pulse::control
{

namespace
{

/// Minimum spacing between two reconcile rounds on the three trackers.
/// A control-plane snapshot is polled by the Qt client at 2 Hz; each
/// reconcile round issues one REST status poll per tracked order on the
/// shared REST mutex, so without this floor the UI would compete with order
/// placement for the mutex. Three seconds is the design document's floor for
/// get_orders and still far faster than the ~10 s background position sync.
constexpr std::int64_t kReconcileIntervalMs = 3000;

/// Clears a single-flight flag and stamps the round's completion time on
/// scope exit, including when a reconcile throws (allocation failure inside a
/// tracker). Stamping the END is deliberate: a round longer than the interval
/// must not re-arm the next one the instant it finishes.
class ReconcileGuard
{
  public:
    ReconcileGuard(std::atomic<bool> &running, std::atomic<std::int64_t> &last)
        : m_running{ running }
        , m_last{ last }
    {
    }

    ~ReconcileGuard()
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        m_last.store(now, std::memory_order_relaxed);
        m_running.store(false, std::memory_order_release);
    }

    ReconcileGuard(const ReconcileGuard &) = delete;
    ReconcileGuard &operator=(const ReconcileGuard &) = delete;

  private:
    std::atomic<bool> &m_running;
    std::atomic<std::int64_t> &m_last;
};

// ---------------------------------------------------------------------------
// open_order idempotency helpers (see OrderIdempotency.hpp)
// ---------------------------------------------------------------------------

/// Render a price/quantity for the idempotency fingerprint at fixed precision.
///
/// Why fixed precision: a retry arrives as freshly parsed JSON, and default
/// ostream formatting already gives the shortest round-tripping form — but
/// nothing guarantees two different parsing paths produce byte-identical
/// text for values that compare equal. Eight fixed decimals removes the
/// question entirely: equal values always format identically, and 1e-8 covers
/// every unit this engine sends (spot sizes, futures contracts, CFD lots,
/// Gate prices are all coarser than 1e-8).
std::string fingerprintNumber(double value)
{
    // Normalise -0.0 to +0.0. The two compare equal but format differently,
    // so a retry carrying -0.0 must not look like a parameter change.
    if (0.0 == value)
    {
        value = 0.0;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(8) << value;
    return out.str();
}

/// Stable fingerprint of everything that changes what the exchange receives.
///
/// Two open_order calls with the same fingerprint describe the SAME order, so
/// a repeated client_order_id is replayed; any difference means the caller
/// reused the key for a different order and must be refused. Fields:
/// symbol, side, quantity, type, price, market_type, leverage, reduce_only,
/// sl_price, tp_price. Derived values (quanto_multiplier) are excluded — they
/// follow from the symbol.
///
/// Enum ordinals are used rather than toString(): the fingerprint never leaves
/// this process, so only stability within one build matters.
std::string orderFingerprint(const execution::OrderRequest &req)
{
    std::ostringstream out;
    out << "symbol=" << req.symbol
        << ";side=" << static_cast<int>(req.side)
        << ";qty=" << fingerprintNumber(req.quantity)
        << ";type=" << static_cast<int>(req.type)
        << ";price=" << fingerprintNumber(req.price)
        << ";market_type=" << static_cast<int>(req.market_type)
        << ";leverage=" << fingerprintNumber(req.leverage)
        << ";reduce_only=" << (req.reduce_only ? 1 : 0)
        << ";sl="
        << (req.sl_price.has_value() ? fingerprintNumber(*req.sl_price)
                                     : std::string{ "none" })
        << ";tp="
        << (req.tp_price.has_value() ? fingerprintNumber(*req.tp_price)
                                     : std::string{ "none" });
    return out.str();
}

/// Terminal-outcome publisher for one idempotency entry.
///
/// Why RAII: after OrderIdempotency::begin() returns New, the entry MUST leave
/// the in-flight state exactly once. Several returns sit between begin() and
/// the order-flow call (the CFD free-margin gate is one), and each of them
/// fails BEFORE anything reached the exchange — so each must abort() rather
/// than cache a rejection. A guard keeps those paths from having to remember
/// it, and covers future early returns too. The destructor also runs if the
/// order flow ever throws, which keeps the key from being stuck in flight
/// until the TTL expires.
class InFlightGuard
{
  public:
    InFlightGuard(OrderIdempotency &cache, std::string key)
        : m_cache{ cache }
        , m_key{ std::move(key) }
    {
    }

    /// Exactly one guard owns the in-flight entry.
    InFlightGuard(const InFlightGuard &) = delete;
    InFlightGuard &operator=(const InFlightGuard &) = delete;

    ~InFlightGuard()
    {
        if (!m_completed)
        {
            m_cache.abort(m_key);
        }
    }

    /// Publish the terminal result (success or a definitive rejection) and
    /// stop owning the entry. Both outcomes are cached: a deterministic
    /// rejection must not be re-executed on a retry either.
    void complete(const Result<execution::OrderResponse> &outcome)
    {
        m_cache.complete(m_key, outcome);
        m_completed = true;
    }

    /// Release the key WITHOUT caching an outcome.
    ///
    /// Only for a request that failed before the order flow could send
    /// anything (rate limiter, drawdown halt, notional/position limits,
    /// missing executor). The key must stay retryable so the caller can
    /// resubmit once the transient condition clears — caching that failure
    /// would pin the key to a rejection that may no longer be true.
    void abort()
    {
        m_cache.abort(m_key);
        m_completed = true;
    }

  private:
    OrderIdempotency &m_cache;
    std::string m_key;
    bool m_completed{ false };
};

/// Whether a failure from the order flow may have left a live order at the
/// exchange.
///
/// Why this matters: an idempotency cache that stores such a failure as a
/// plain rejection replays "definitely not placed" for the whole TTL, so the
/// caller mints a new key and submits again — a second position while the
/// first one is live and unknown to everyone. These failures are therefore
/// cached as an explicit "outcome unknown — reconcile" result instead.
///
/// Classification is by the transport semantics of GateRestClient:
///   - a curl timeout / connection loss means the POST was on the wire and
///     Gate may have accepted it before the transport failed;
///   - HTTP failures are rendered as "HTTP <status>: ..." and both 4xx and
///     5xx map to HttpError, but only a 5xx can have created the order (a 4xx
///     is a definitive exchange rejection).
bool orderMayHaveReachedExchange(const PulseError &err)
{
    switch (err.code)
    {
    case ErrorCode::NetworkTimeout:
    case ErrorCode::NetworkDisconnected:
        return true;
    case ErrorCode::HttpError:
        return 0 == err.message.rfind("HTTP 5", 0);
    default:
        return false;
    }
}

/// Whether a failure is guaranteed to have happened before any order could be
/// sent, so the key must stay retryable rather than be pinned to it.
///
/// The list mirrors the pre-flight gates of the order flow: the risk manager
/// (rate limiter, drawdown halt, notional/position limits, leverage/margin
/// checks), the manual direction gate, and the executor lookup. OrderRejected
/// is included because both of its sources are safe to observe again: a risk
/// veto before the POST, and an exchange business rejection of a CFD order
/// (which provably created nothing).
bool isPreFlightRejection(ErrorCode code)
{
    switch (code)
    {
    case ErrorCode::RateLimitHit:
    case ErrorCode::DrawdownLimitHit:
    case ErrorCode::ManualHalt:
    case ErrorCode::PositionLimitHit:
    case ErrorCode::SymbolLimitHit:
    case ErrorCode::InactiveMarket:
    case ErrorCode::OrderRejected:
    case ErrorCode::InternalError:
    case ErrorCode::FuturesLeverageExceeded:
    case ErrorCode::FuturesMarginInsufficient:
    case ErrorCode::CfdLeverageExceeded:
    case ErrorCode::CfdMarginInsufficient:
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// StrategyParams field dispatch tables
// ---------------------------------------------------------------------------
using ParamGetter = std::function<double(const strategy::StrategyParams &)>;
using ParamSetter = std::function<void(strategy::StrategyParams &, double)>;

const std::unordered_map<std::string, ParamGetter> &paramGetters()
{
    static const std::unordered_map<std::string, ParamGetter> table = {
        { "order_quantity",      [](const auto &p) { return p.order_quantity.load(); } },
        { "min_confidence",      [](const auto &p) { return p.min_confidence.load(); } },
        { "ema_fast_period",     [](const auto &p) { return p.ema_fast_period.load(); } },
        { "ema_slow_period",     [](const auto &p) { return p.ema_slow_period.load(); } },
        { "bb_period",           [](const auto &p) { return p.bb_period.load(); } },
        { "bb_std_dev",          [](const auto &p) { return p.bb_std_dev.load(); } },
        { "ob_imbalance_threshold", [](const auto &p) { return p.ob_imbalance_threshold.load(); } },
        { "ob_depth",            [](const auto &p) { return p.ob_depth.load(); } },
        { "supertrend_period",   [](const auto &p) { return p.supertrend_period.load(); } },
        { "supertrend_multiplier", [](const auto &p) { return p.supertrend_multiplier.load(); } },
        { "cooldown_seconds",    [](const auto &p) { return p.cooldown_seconds.load(); } },
        { "stop_loss_pct",       [](const auto &p) { return p.stop_loss_pct.load(); } },
        { "take_profit_pct",     [](const auto &p) { return p.take_profit_pct.load(); } },
        { "auto_trade",          [](const auto &p) { return p.auto_trade.load(); } },
    };
    return table;
}

const std::unordered_map<std::string, ParamSetter> &paramSetters()
{
    static const std::unordered_map<std::string, ParamSetter> table = {
        { "order_quantity",      [](auto &p, double v) { p.order_quantity.store(v); } },
        { "min_confidence",      [](auto &p, double v) { p.min_confidence.store(v); } },
        { "ema_fast_period",     [](auto &p, double v) { p.ema_fast_period.store(v); } },
        { "ema_slow_period",     [](auto &p, double v) { p.ema_slow_period.store(v); } },
        { "bb_period",           [](auto &p, double v) { p.bb_period.store(v); } },
        { "bb_std_dev",          [](auto &p, double v) { p.bb_std_dev.store(v); } },
        { "ob_imbalance_threshold", [](auto &p, double v) { p.ob_imbalance_threshold.store(v); } },
        { "ob_depth",            [](auto &p, double v) { p.ob_depth.store(v); } },
        { "supertrend_period",   [](auto &p, double v) { p.supertrend_period.store(v); } },
        { "supertrend_multiplier", [](auto &p, double v) { p.supertrend_multiplier.store(v); } },
        { "cooldown_seconds",    [](auto &p, double v) { p.cooldown_seconds.store(v); } },
        { "stop_loss_pct",       [](auto &p, double v) { p.stop_loss_pct.store(v); } },
        { "take_profit_pct",     [](auto &p, double v) { p.take_profit_pct.store(v); } },
        { "auto_trade",          [](auto &p, double v) { p.auto_trade.store(v); } },
    };
    return table;
}

/// Extract "USDT" account from GET /spot/accounts array JSON.
nlohmann::json spotUsdtAccount(const nlohmann::json &accounts)
{
    nlohmann::json result = {
        { "available", 0.0 },
        { "total", 0.0 },
        { "currency", "USDT" },
    };
    if (!accounts.is_array())
    {
        return result;
    }
    for (const auto &acc : accounts)
    {
        const auto currency = acc.value("currency", "");
        if ("USDT" != currency)
        {
            continue;
        }
        const double available = safeParseDouble(acc.value("available", "0"))
                                     .value_or(0.0);
        const double locked = safeParseDouble(acc.value("locked", "0"))
                                  .value_or(0.0);
        result["available"] = available;
        result["total"] = available + locked;
        break;
    }
    return result;
}

/// Full non-zero spot holdings with a best-effort USDT valuation.
///
/// `priceLookup` returns the USDT price of a currency, or 0.0 when none is
/// available. A zero price keeps the holding in the list with
/// price_usdt/value_usdt = null, so the UI can show the amount and a dash
/// rather than inventing a value.
///
/// Entries are sorted by value descending; unpriced entries go last.
nlohmann::json spotBalances(
    const nlohmann::json &accounts,
    const std::function<double(const std::string &)> &priceLookup)
{
    nlohmann::json out = nlohmann::json::array();

    if (!accounts.is_array())
    {
        return out;
    }

    for (const auto &acc : accounts)
    {
        const std::string currency = acc.value("currency", "");
        const double available =
            safeParseDouble(acc.value("available", "0")).value_or(0.0);
        const double locked =
            safeParseDouble(acc.value("locked", "0")).value_or(0.0);
        const double total = available + locked;

        // Dust and zero balances would bury the meaningful rows.
        if (currency.empty() || total <= 0.0)
        {
            continue;
        }

        const double price = priceLookup(currency);
        const bool priced = price > 0.0;

        nlohmann::json item = {
            { "currency", currency },
            { "available", available },
            { "locked", locked },
            { "total", total },
            { "price_usdt", priced ? nlohmann::json(price)
                                   : nlohmann::json(nullptr) },
            { "value_usdt", priced ? nlohmann::json(total * price)
                                   : nlohmann::json(nullptr) },
        };
        out.push_back(std::move(item));
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const nlohmann::json &lhs, const nlohmann::json &rhs)
    {
        const double left = lhs["value_usdt"].is_number()
                                ? lhs["value_usdt"].get<double>()
                                : -1.0;
        const double right = rhs["value_usdt"].is_number()
                                 ? rhs["value_usdt"].get<double>()
                                 : -1.0;
        return left > right;
    });

    return out;
}

} // anonymous namespace

double EngineServices::spotPriceUsdt(const std::string &currency)
{
    // 1. USDT is the quote currency, so its value is exactly 1.0.
    if ("USDT" == currency)
    {
        return 1.0;
    }

    const std::string symbol = currency + "_USDT";

    // 2. Live ticker caches: spot first, then futures (some coins are listed
    //    only as perpetuals).
    if (nullptr != m_spotFeed)
    {
        const auto ticker = m_spotFeed->tickerCache().get(symbol);

        if (ticker.has_value() && ticker->last > 0.0)
        {
            return ticker->last;
        }
    }

    if (nullptr != m_futuresFeed)
    {
        const auto ticker = m_futuresFeed->tickerCache().get(symbol);

        if (ticker.has_value() && ticker->last > 0.0)
        {
            return ticker->last;
        }
    }

    // 3. Cached REST result. A found price lives 60 s; a failed lookup 300 s,
    //    so a delisted or unpaired token does not cost a call on every poll.
    const auto now = std::chrono::steady_clock::now();
    const auto cached = m_spotPrices.find(symbol);

    if (cached != m_spotPrices.end())
    {
        const auto ttl = cached->second.price > 0.0
                             ? std::chrono::seconds(60)
                             : std::chrono::seconds(300);

        if (now - cached->second.fetched < ttl)
        {
            return cached->second.price;
        }
    }

    // 4. Bound the REST lookups per snapshot: a wallet with many unpriced
    //    altcoins must not turn one get_account into dozens of calls.
    constexpr std::size_t kMaxLookupsPerSnapshot = 8;

    if (m_spotPriceLookups >= kMaxLookupsPerSnapshot)
    {
        return 0.0;
    }

    ++m_spotPriceLookups;

    double price = 0.0;

    if (nullptr != m_spotRest)
    {
        auto result = m_spotRest->getTicker(symbol);

        if (ok(result))
        {
            const auto &body = value(result);

            if (body.is_array() && !body.empty())
            {
                price = safeParseDouble(body.front().value("last", "0"))
                            .value_or(0.0);
            }
        }
    }

    if (price < 0.0)
    {
        price = 0.0;
    }

    m_spotPrices[symbol] = SpotPrice{ price, now };
    return price;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
EngineServices::EngineServices(
    std::string version,
    std::chrono::steady_clock::time_point engine_start,
    const PulseConfig &cfg,
    strategy::StrategyManager &strategy_mgr,
    risk::RiskManager &risk_mgr,
    risk::PositionManager &position_mgr,
    market::MarketFeed *spot_feed,
    market::MarketFeed *futures_feed,
    market::MarketFeed *cfd_feed,
    exchange::GateRestClient *spot_rest,
    exchange::GateRestClient *futures_rest,
    exchange::GateRestClient *cfd_rest,
    execution::OrderTracker *spot_tracker,
    execution::OrderTracker *futures_tracker,
    execution::OrderTracker *cfd_tracker,
    OrderFlowExecutor &order_flow,
    strategy::SignalBoard &signal_board,
    std::mutex &rest_mutex,
    const std::shared_ptr<market::SymbolRegistry> &registry,
    grid::GridManager *grid,
    core::ParamChangeLog *param_change_log,
    trade_recorder::TradeRecorder *trade_recorder)
    : m_version{ std::move(version) }
    , m_engineStart{ engine_start }
    , m_cfg{ cfg }
    , m_externalOrdersEnabled{ cfg.control.externalOrdersEnabled }
    , m_strategyMgr{ strategy_mgr }
    , m_riskMgr{ risk_mgr }
    , m_positionMgr{ position_mgr }
    , m_spotFeed{ spot_feed }
    , m_futuresFeed{ futures_feed }
    , m_cfdFeed{ cfd_feed }
    , m_spotRest{ spot_rest }
    , m_futuresRest{ futures_rest }
    , m_cfdRest{ cfd_rest }
    , m_spotTracker{ spot_tracker }
    , m_futuresTracker{ futures_tracker }
    , m_cfdTracker{ cfd_tracker }
    , m_orderFlow{ order_flow }
    , m_grid{ grid }
    , m_signalBoard{ signal_board }
    , m_restMutex{ rest_mutex }
    , m_paramChangeLog{ param_change_log }
    , m_tradeRecorder{ trade_recorder }
    , m_displayTz{ parseDisplayTimezone(cfg.control.displayTimezone)
                       .value_or(DisplayTimezone::local()) }
    , m_registry{ registry }
{
    // Emit the `submitted` push event from INSIDE the order flow. Emitting it
    // after openOrder() returns would publish one event per order but in the
    // wrong order: a market order can fill synchronously in the tracker's
    // immediate poll, so `completed` would reach subscribers before
    // `submitted`. The sink is installed here (not by main) because only this
    // class knows how to serialize the event and reach the control socket.
    m_orderFlow.setOrderSubmitCallback(
        [this](const execution::OrderRequest &placed,
               const execution::OrderResponse &resp)
        { publishOrderSubmitted(placed, resp); });
}

market::MarketFeed *EngineServices::feedFor(MarketType mt) const
{
    switch (mt)
    {
    case MarketType::Futures:
        return m_futuresFeed;
    case MarketType::Cfd:
        return m_cfdFeed;
    default:
        return m_spotFeed;
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
nlohmann::json EngineServices::status() const
{
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_engineStart).count();

    nlohmann::json j;
    j["version"] = m_version;
    j["uptime_sec"] = uptime;
    j["network"] = m_cfg.exchange.testnet ? "testnet" : "mainnet";
    // Report the watchlist as well, so a client dropdown offers every symbol
    // the engine actually streams in the active market.
    std::vector<std::string> reportedSymbols = m_cfg.symbols;

    for (const auto& symbol : m_cfg.watch_symbols)
    {
        if (std::find(reportedSymbols.begin(), reportedSymbols.end(), symbol)
            == reportedSymbols.end())
        {
            reportedSymbols.push_back(symbol);
        }
    }

    j["symbols"] = reportedSymbols;
    j["strategies_running"] = m_strategyMgr.runningCount();
    j["strategies_total"] = m_strategyMgr.strategyCount();
    j["open_positions"] = m_positionMgr.openPositionCount();
    j["trading_halted"] = m_riskMgr.isTradingHalted();
    j["active_market"] = toString(m_orderFlow.activeMarket());
    // Independent external-order gate state: a client should be able to tell
    // why a caller-authoritative open is being refused without reading TOML.
    j["external_orders_enabled"] =
        m_externalOrdersEnabled.load(std::memory_order_relaxed);

    if (m_spotFeed)
    {
        j["feed_spot"] = m_spotFeed->stats();
    }
    if (m_futuresFeed)
    {
        j["feed_futures"] = m_futuresFeed->stats();
    }
    if (m_cfdFeed)
    {
        j["feed_cfd"] = m_cfdFeed->stats();
    }
    return j;
}

nlohmann::json EngineServices::account(bool include_spot_balances)
{
    nlohmann::json j;
    j["available"] = false;

    std::lock_guard lock(m_restMutex);

    // Futures account (primary for futures strategies).
    if (m_futuresRest)
    {
        auto fut = m_futuresRest->getFuturesAccountBalance();
        if (ok(fut))
        {
            const auto &bal = value(fut);
            j["available"] = true;
            j["total"] = bal.total;
            j["available_balance"] = bal.available;
            j["unrealised_pnl"] = bal.unrealised_pnl;
            j["position_margin"] = bal.position_margin;
            j["order_margin"] = bal.order_margin;
            j["currency"] = bal.currency;
        }
    }

    // Spot account (USDT portion).
    if (m_spotRest)
    {
        auto spot = m_spotRest->getSpotAccounts();
        if (ok(spot))
        {
            // `spot` stays the aggregate USDT view for backward compatibility.
            // The per-currency breakdown is opt-in: pricing an unpriced coin
            // costs a REST call under the shared mutex, which delays every
            // other REST consumer for as long as the calls take.
            j["spot"] = spotUsdtAccount(value(spot));

            if (include_spot_balances)
            {
                // The lookup budget resets once per snapshot.
                m_spotPriceLookups = 0;
                j["spot_balances"] = spotBalances(
                    value(spot),
                    [this](const std::string &currency)
                    {
                        return spotPriceUsdt(currency);
                    });
            }
        }
    }

    // TradFi CFD account (USD settlement, MT5 account).
    if (m_cfdRest)
    {
        auto cfd = m_cfdRest->getCfdAssets();
        if (ok(cfd))
        {
            const auto &data = value(cfd).value("data", nlohmann::json::object());
            j["cfd"] = {
                { "total",    safeParseDouble(data.value("equity", "0")).value_or(0.0) },
                { "available", safeParseDouble(data.value("margin_free", "0")).value_or(0.0) },
                { "margin",   safeParseDouble(data.value("margin", "0")).value_or(0.0) },
                { "unrealised_pnl", safeParseDouble(data.value("unrealized_pnl", "0")).value_or(0.0) },
                { "currency", "USD" },
            };
        }
    }

    return j;
}

void EngineServices::reconcileTrackersThrottled() const
{
    // 1. Single-flight first. The interval check alone is a start-time rate
    //    limiter: a round slower than the interval (a stalled REST call has a
    //    10 s timeout with retries) would be re-entered by the next poll from
    //    another control connection, doubling REST traffic on the shared
    //    client exactly when the exchange is already slow.
    if (m_reconcileRunning.exchange(true, std::memory_order_acquire))
    {
        return;
    }
    const ReconcileGuard guard(m_reconcileRunning, m_lastReconcileMs);

    // 2. Rate limit, measured from the completion of the previous round (the
    //    guard stamps it). This is the path almost every 2 Hz UI poll takes,
    //    which is exactly the point — a snapshot poll must not turn into REST
    //    traffic on every request. Measuring from completion also keeps a
    //    round longer than the interval from running back-to-back.
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const std::int64_t previous =
        m_lastReconcileMs.load(std::memory_order_relaxed);
    if (now - previous < kReconcileIntervalMs)
    {
        return;
    }

    // 3. The tracker holds the shared REST mutex around each individual
    //    status poll, not for the whole round: GateRestClient is not
    //    thread-safe, but a per-round lock would block order placement for
    //    one HTTP timeout per tracked order. Lock order matches
    //    OrderFlowExecutor: REST mutex first, executor state second (the
    //    completion callback takes the executor mutex).
    if (m_spotTracker)
    {
        m_spotTracker->reconcileAll(m_restMutex);
    }
    if (m_futuresTracker)
    {
        m_futuresTracker->reconcileAll(m_restMutex);
    }
    if (m_cfdTracker)
    {
        m_cfdTracker->reconcileAll(m_restMutex);
    }
}

nlohmann::json EngineServices::positions(bool reconcile) const
{
    // Reconcile first so fills that missed the WS private channel (market
    // orders fill instantly, before the subscription exists) surface as
    // positions before the snapshot is taken. Throttled: see
    // reconcileTrackersThrottled().
    if (reconcile)
    {
        reconcileTrackersThrottled();
    }
    nlohmann::json j;
    j["positions"] = m_positionMgr.getAllPositions();
    j["portfolio"] = m_positionMgr.portfolioSummary();

    // Human-readable timestamps in the configured display timezone. The raw
    // epoch-ms fields stay untouched (machine-readable, TZ-independent); the
    // *_str companions make the times comparable with a phone app that shows
    // a different timezone (e.g. US time vs Beijing time).
    for (auto &p : j["positions"])
    {
        p["open_time_str"] = formatEpochMs(p.value("open_time", 0LL), m_displayTz);
    }
    return j;
}

nlohmann::json EngineServices::orders(bool reconcile) const
{
    // Same reconcile as positions(): present tracked orders' real state
    // instead of a stale Pending from before the WS subscription existed.
    // Throttled: see reconcileTrackersThrottled().
    if (reconcile)
    {
        reconcileTrackersThrottled();
    }
    nlohmann::json j;

    // The three trackers are merged here, so market identity is lost unless
    // it is stamped while the source is still known. The UI needs it to put
    // an order on the spot / CFD / futures tab; without it both lists are a
    // single indistinguishable pool (the reported dashboard problem).
    nlohmann::json active = nlohmann::json::array();
    nlohmann::json reports = nlohmann::json::array();
    const auto appendTracker = [&active, &reports](execution::OrderTracker *tracker,
                                                   const char *market)
    {
        if (nullptr == tracker)
        {
            return;
        }

        // 1. Active orders: OrderSnapshot::to_json (ADL) plus market_type.
        for (const auto &order : tracker->activeOrders())
        {
            nlohmann::json entry;
            to_json(entry, order);
            entry["market_type"] = market;
            active.push_back(std::move(entry));
        }

        // 2. Recent fills: ExecutionReport::to_json (ADL) plus market_type.
        //    The report struct is per-tracker too, so the tag is exact.
        for (const auto &report : tracker->recentReports(20))
        {
            nlohmann::json entry;
            to_json(entry, report);
            entry["market_type"] = market;
            reports.push_back(std::move(entry));
        }
    };
    appendTracker(m_spotTracker, "spot");
    appendTracker(m_futuresTracker, "futures");
    appendTracker(m_cfdTracker, "cfd");

    j["activeOrders"] = std::move(active);
    j["recentReports"] = std::move(reports);

    // Human-readable timestamps in the configured display timezone (see
    // positions()). Note: OrderSnapshot timestamps are epoch MILLIseconds,
    // ExecutionReport timestamps are epoch NANOSECONDS — normalize both.
    for (auto &o : j["activeOrders"])
    {
        o["submit_time_str"] = formatEpochMs(o.value("submit_time", 0LL), m_displayTz);
        o["last_update_time_str"] = formatEpochMs(
            o.value("last_update_time", 0LL), m_displayTz);
    }
    for (auto &r : j["recentReports"])
    {
        r["submit_time_str"] = formatEpochMs(
            r.value("submit_time", 0LL) / 1000000LL, m_displayTz);
        r["fill_time_str"] = formatEpochMs(
            r.value("fill_time", 0LL) / 1000000LL, m_displayTz);
    }
    return j;
}

nlohmann::json EngineServices::strategies() const
{
    return m_strategyMgr.snapshot();
}

nlohmann::json EngineServices::signals() const
{
    auto snap = m_signalBoard.snapshot();

    // Decorate every entry with a display-timezone human-readable timestamp.
    const auto decorate = [this](nlohmann::json &entry)
    {
        if (entry.is_object() && entry.contains("ts_ms"))
        {
            entry["ts_str"] = formatEpochMs(
                entry["ts_ms"].get<std::int64_t>(), m_displayTz);
        }
    };
    for (auto &sig : snap["signals"])
    {
        decorate(sig);
    }
    if (snap.contains("aggregate") && !snap["aggregate"].is_null())
    {
        decorate(snap["aggregate"]);
    }
    return snap;
}

nlohmann::json EngineServices::getStrategyParams(const std::string &id) const
{
    auto *params = m_strategyMgr.paramsByName(id);
    if (nullptr == params)
    {
        return nullptr;
    }
    return strategyParamsJson(*params);
}

bool EngineServices::setStrategyParam(const std::string &id,
                                      const std::string &param,
                                      double value)
{
    auto *params = m_strategyMgr.paramsByName(id);
    if (nullptr == params)
    {
        return false;
    }
    const auto &setters = paramSetters();
    const auto it = setters.find(param);
    if (setters.end() == it)
    {
        return false;
    }

    // Clamp to the shared safety bounds (same table the AI pipeline uses) —
    // parameters without an entry (ob_depth/supertrend_*) pass through.
    const auto &bounds = ai::defaultParamBounds();
    const auto bit = bounds.find(param);
    double clamped = value;
    if (bounds.end() != bit)
    {
        const auto &b = bit->second;
        clamped = std::clamp(value, b.hard_min, b.hard_max);
        if (std::abs(clamped - value) > 1e-12)
        {
            PULSE_LOG_WARN("control", "set_strategy_param {} {}: {:.6f} clamped "
                           "to [{:.6f}, {:.6f}] → {:.6f}", id, param, value,
                           b.hard_min, b.hard_max, clamped);
        }
    }

    // Audit before writing (read the old value first).
    const auto old_value = paramGetters().at(param)(*params);
    it->second(*params, clamped);
    if (m_paramChangeLog != nullptr)
    {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        m_paramChangeLog->record({ now_ns, id, param, old_value, clamped,
                                   "manual" });
    }
    return true;
}

nlohmann::json EngineServices::paramHistory() const
{
    nlohmann::json arr = nlohmann::json::array();
    if (nullptr == m_paramChangeLog)
    {
        return arr;
    }
    for (const auto &entry : m_paramChangeLog->snapshot())
    {
        arr.push_back({ { "ts_ns", entry.ts_ns },
                        { "strategy_id", entry.strategy_id },
                        { "param_name", entry.param_name },
                        { "old_value", entry.old_value },
                        { "new_value", entry.new_value },
                        { "source", entry.source } });
    }
    return arr;
}

Result<nlohmann::json> EngineServices::strategyPerformance(int hours) const
{
#ifdef PULSE_ENABLE_SQLITE
    if (nullptr == m_tradeRecorder)
    {
        return PulseError{ ErrorCode::TradeRecorderNotOpen,
                           "trade recorder not wired (SQLite disabled?)" };
    }
    if (hours < 1)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "get_strategy_performance: hours must be ≥ 1" };
    }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto summary = m_tradeRecorder->getStrategySummary(
        now_ns - static_cast<std::int64_t>(hours) * 3600LL * 1000'000'000LL,
        now_ns);
    if (!ok(summary))
    {
        return error(summary);
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &row : value(summary))
    {
        arr.push_back({ { "strategy_name", row.strategy_name },
                        { "market_type", row.market_type },
                        { "trade_count", row.trade_count },
                        { "total_pnl", row.total_pnl },
                        { "win_rate", row.win_rate },
                        { "total_fees", row.total_fees } });
    }
    return arr;
#else
    (void)hours;
    return PulseError{ ErrorCode::TradeRecorderNotOpen,
                       "built without PULSE_ENABLE_SQLITE" };
#endif
}

nlohmann::json EngineServices::risk() const
{
    return m_riskMgr.riskSnapshot();
}

void EngineServices::setGoldTrader(const risk::GoldTrader *trader,
                                   const GoldTraderConfig &config,
                                   std::string disabled_reason)
{
    m_goldTrader = trader;
    m_goldConfig = config;
    m_goldDisabledReason = std::move(disabled_reason);
}

void EngineServices::setOrderEventSink(OrderEventSink sink)
{
    m_orderEventSink = std::move(sink);
}

void EngineServices::publishOrderSubmitted(
    const execution::OrderRequest &req,
    const execution::OrderResponse &resp)
{
    // Only a successful placement reaches this point: OrderFlowExecutor calls
    // the sink after placer->place() returned an OrderResponse, and an
    // idempotent replay returns before the order flow is entered — so one
    // exchange order produces exactly one `submitted` event.
    if (!m_orderEventSink)
    {
        return;
    }

    // The sink runs inside the order flow, which runs inside a control-session
    // handler. A throwing sink must never turn a placed order into an error
    // reply to the caller.
    try
    {
        nlohmann::json event;
        event["event"] = "submitted";
        event["order_id"] = resp.order_id;
        event["client_order_id"] = req.client_order_id;
        event["symbol"] = req.symbol;
        event["market_type"] = toString(req.market_type);
        event["side"] = (Side::Buy == req.side) ? "buy" : "sell";
        event["type"] = toString(req.type);
        event["price"] = req.price;
        // The request passed to the sink is the one the order flow actually
        // sent: placeOrder() rewrites `quantity` when risk approves a smaller
        // size, so this is the post-adjustment size — matching what the
        // `completed` event reports as requested_quantity.
        event["quantity"] = req.quantity;
        // The submit event never carries a terminal status. A market order's
        // POST reply can already be Filled (futures IOC / spot closed), but
        // nothing may be reported as a completed fill here: the terminal
        // report follows as `completed`, and a client treating this event as
        // terminal would record a zero-size fill and re-arm the entry.
        const auto status =
            (OrderStatus::Filled == resp.status
             || OrderStatus::Cancelled == resp.status)
                ? OrderStatus::Open
                : resp.status;
        event["filled_quantity"] = 0.0;
        event["status"] = execution::orderStatusToString(status);
        event["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now().time_since_epoch())
                          .count();
        m_orderEventSink(event);
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_WARN("control",
                       "open_order: order event sink threw: {}", e.what());
    }
    catch (...)
    {
        PULSE_LOG_WARN("control",
                       "open_order: order event sink threw a non-std exception");
    }
}

nlohmann::json EngineServices::goldTraderStatus() const
{
    if (nullptr != m_goldTrader)
    {
        return m_goldTrader->status();
    }

    // Disabled / failed-to-arm path: still report the configured thresholds
    // so the card can explain what WOULD run and why it does not.
    nlohmann::json status;
    status["armed"] = false;
    status["enabled"] = m_goldConfig.enabled;
    status["symbol"] = m_goldConfig.symbol;
    status["market_type"] = toString(m_goldConfig.market_type);
    status["reason"] = m_goldDisabledReason;
    status["config"] = risk::GoldTrader::configJson(m_goldConfig);
    status["tracked_positions"] = nlohmann::json::array();
    return status;
}

nlohmann::json EngineServices::market(const std::string &symbol,
                                      int book_levels,
                                      int klines,
                                      const std::string &market_type) const
{
    nlohmann::json j;
    j["symbol"] = symbol;

    // Pick feed: explicit market_type wins; else the active direction;
    // else futures by default for futures strategies, else spot.
    market::MarketFeed *feed = nullptr;
    if (!market_type.empty())
    {
        const auto mt = parseMarketType(market_type);
        if (!mt.has_value())
        {
            j["error"] = "market_type must be spot/futures/cfd";
            return j;
        }
        feed = feedFor(*mt);
    }
    else
    {
        feed = feedFor(m_orderFlow.activeMarket());
        if (nullptr == feed)
        {
            feed = m_futuresFeed ? m_futuresFeed : m_spotFeed;
        }
    }
    if (nullptr == feed)
    {
        j["error"] = "no market feed available";
        return j;
    }

    // Ticker.
    auto ticker = feed->tickerCache().get(symbol);
    if (ticker.has_value())
    {
        j["ticker"] = *ticker;
    }

    // Order book.
    nlohmann::json book;
    book["bids"] = feed->orderbookManager().topBids(symbol, book_levels);
    book["asks"] = feed->orderbookManager().topAsks(symbol, book_levels);
    j["order_book"] = book;

    // Klines.
    if (klines > 0)
    {
        j["klines"] = feed->getKlineBuffer(symbol).snapshot(klines);
    }

    return j;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
Result<execution::OrderResponse>
EngineServices::openOrder(const nlohmann::json &params)
{
    if (!params.is_object())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "open_order: params must be an object" };
    }

    execution::OrderRequest req;

    // Required: symbol.
    const auto symbol = params.value("symbol", "");
    if (symbol.empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "open_order: symbol is required" };
    }
    req.symbol = symbol;

    // Required: side ("buy" / "sell").
    const auto side = parseSide(params.value("side", ""));
    if (!side.has_value())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "open_order: side must be \"buy\" or \"sell\"" };
    }
    req.side = *side;

    // Required: quantity (number).
    if (!params.contains("quantity") || !params["quantity"].is_number())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "open_order: quantity (number) is required" };
    }
    req.quantity = params["quantity"].get<double>();

    // Optional: type / price / market_type / leverage / reduce_only / client_order_id.
    if (params.contains("type"))
    {
        const auto type = parseOrderType(params["type"].get<std::string>());
        if (!type.has_value())
        {
            return PulseError{ ErrorCode::ControlInvalidRequest,
                               "open_order: type must be market/limit/post_only" };
        }
        req.type = *type;
    }
    if (params.contains("price"))
    {
        req.price = params["price"].get<double>();
    }
    if (params.contains("market_type"))
    {
        const auto mt = parseMarketType(params["market_type"].get<std::string>());
        if (!mt.has_value())
        {
            return PulseError{ ErrorCode::ControlInvalidRequest,
                               "open_order: market_type must be spot/futures/cfd" };
        }
        req.market_type = *mt;
    }
    else
    {
        // Omitted market_type defaults to the active trading direction —
        // otherwise the default (Spot) is rejected by the direction gate.
        req.market_type = m_orderFlow.activeMarket();
    }
    if (params.contains("leverage"))
    {
        req.leverage = params["leverage"].get<double>();
    }
    if (params.contains("reduce_only"))
    {
        req.reduce_only = params["reduce_only"].get<bool>();
    }
    // reduce_only is only meaningful where the exchange enforces it: the
    // FUTURES order body carries the flag, the spot and CFD bodies do not
    // (OrderExecutor::buildOrderBody). Accepting it on those markets would
    // silently turn a claimed "close" into new exposure AND exempt the order
    // from the risk layer's drawdown halt and position-count checks (both key
    // off the same flag), while the exchange never sees it. Reject loudly —
    // close_position is the close path for spot and CFD.
    if (req.reduce_only && MarketType::Futures != req.market_type)
    {
        return PulseError{
            ErrorCode::ControlInvalidRequest,
            "open_order: reduce_only is futures-only; use close_position for "
            "spot and CFD positions." };
    }
    if (params.contains("client_order_id"))
    {
        req.client_order_id = params["client_order_id"].get<std::string>();
    }
    // Optional: size authority. "caller" marks `quantity` authoritative — the
    // risk layer may reject the order but must never shrink it. An external
    // strategy runtime (GT) derives its own position size from its own risk
    // rules, and a silently reduced fill would invalidate its stop distance
    // and R-multiple. "engine" (default) preserves the historical
    // reduce-to-fit behaviour used by signal-driven and manual orders.
    if (params.contains("size_authority"))
    {
        if (!params["size_authority"].is_string())
        {
            return PulseError{ ErrorCode::ControlInvalidRequest,
                               "open_order: size_authority must be \"caller\" or \"engine\"" };
        }
        const auto authority = params["size_authority"].get<std::string>();
        if ("caller" == authority)
        {
            req.size_authoritative = true;
        }
        else if ("engine" != authority)
        {
            return PulseError{ ErrorCode::ControlInvalidRequest,
                               "open_order: size_authority must be \"caller\" or \"engine\"" };
        }
    }

    // Optional: attached stop-loss / take-profit (CFD only — TradFi orders
    // accept per-order price_sl / price_tp; futures/spot have no equivalent
    // on this path, so reject loudly rather than silently dropping).
    if (params.contains("sl_price"))
    {
        req.sl_price = params["sl_price"].get<double>();
    }
    if (params.contains("tp_price"))
    {
        req.tp_price = params["tp_price"].get<double>();
    }
    if ((req.sl_price.has_value() || req.tp_price.has_value())
        && MarketType::Cfd != req.market_type)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "open_order: sl_price/tp_price are CFD-only" };
    }

    // Independent external-order gate ([control] external_orders_enabled).
    // It sits here because every request field is parsed (caller authority
    // and reduce_only are known) and nothing has touched the risk gate, REST
    // or the idempotency cache yet. Why:
    //   1. Only caller-authoritative opens are gated — that is the path an
    //      external strategy runtime (GT) uses because it sizes its own
    //      positions. Engine-authority orders (REPL / MCP / UI one-shots)
    //      stay live, so the manual path never depends on this flag.
    //   2. reduce_only is exempt only where the EXCHANGE enforces it. The
    //      futures order body carries `reduce_only`; the CFD and spot bodies
    //      do not (OrderExecutor::buildOrderBody), and open_order now rejects
    //      the flag for those markets outright — so the exemption below can
    //      only ever apply to a futures close, which the exchange itself
    //      bounds by the open position.
    //   3. Failing BEFORE the idempotency cache matters: a refusal does not
    //      consume the client_order_id, so after `set_external_orders true`
    //      the caller can retry the same request and actually place it
    //      instead of replaying a cached rejection.
    const bool enforced_reduce_only =
        req.reduce_only && MarketType::Futures == req.market_type;
    if (req.size_authoritative && !enforced_reduce_only
        && !m_externalOrdersEnabled.load(std::memory_order_relaxed))
    {
        return PulseError{
            ErrorCode::ControlInvalidRequest,
            "open_order: external orders are disabled ([control] "
            "external_orders_enabled = false); caller-authoritative opens "
            "are rejected, futures reduce_only closes and engine-sized "
            "orders still work." };
    }

    // Idempotency gate — the full contract is documented on the declaration in
    // EngineServices.hpp. It sits here because all fingerprint fields are now
    // parsed and the cheap validation is done, while nothing has touched REST
    // or the risk gate yet: a duplicate must be refused before either.
    //
    // 1. A caller-authoritative size without a key cannot be de-duplicated:
    //    a retry after a timeout would place a second order of exactly the
    //    size the caller insisted on. Only a FUTURES reduce_only close is
    //    exempt — there the exchange enforces the flag, so the order is
    //    bounded by the open position even without a key. spot/CFD
    //    reduce_only requests were already rejected above.
    if (req.size_authoritative && !enforced_reduce_only
        && req.client_order_id.empty())
    {
        return PulseError{
            ErrorCode::ControlInvalidRequest,
            "open_order: client_order_id is required when size_authority is "
            "\"caller\"." };
    }

    // 2. With a key, consult the cache. Keyless requests keep the historical
    //    fire-and-forget behaviour (REPL / MCP / UI one-shots that have no
    //    retry story of their own).
    std::optional<InFlightGuard> guard;
    if (!req.client_order_id.empty())
    {
        switch (m_orderIdempotency.begin(req.client_order_id,
                                        orderFingerprint(req)))
        {
            case OrderIdempotency::Decision::InFlight:
                return PulseError{
                    ErrorCode::ControlInvalidRequest,
                    "open_order: duplicate client_order_id is already in "
                    "flight." };
            case OrderIdempotency::Decision::Conflict:
                return PulseError{
                    ErrorCode::ControlInvalidRequest,
                    "open_order: client_order_id reused with different order "
                    "parameters." };
            case OrderIdempotency::Decision::Replay:
            {
                // The first attempt reached a terminal outcome. Return it
                // verbatim — the same order_id for a success, the same
                // PulseError for a rejection — and send nothing.
                auto cached =
                    m_orderIdempotency.cachedOutcome(req.client_order_id);
                if (cached.has_value())
                {
                    return *cached;
                }
                // Unreachable: begin() reports Replay only for a completed
                // entry. Fail loudly instead of placing a possibly duplicate
                // order if that invariant ever breaks.
                return PulseError{
                    ErrorCode::InternalError,
                    "open_order: idempotency replay lost its cached outcome" };
            }
            case OrderIdempotency::Decision::New:
                guard.emplace(m_orderIdempotency, req.client_order_id);
                break;
            case OrderIdempotency::Decision::Capacity:
                // Fail closed: the cache holds 1024 live keys (guards and
                // replayable outcomes) and dropping one would trade a
                // guaranteed-safe refusal for a possible duplicate order.
                // ControlBusy marks the refusal as retryable.
                return PulseError{
                    ErrorCode::ControlBusy,
                    "open_order: idempotency cache is full; retry shortly." };
        }
    }

    // Futures qty is in contracts — the risk gate needs the contract
    // multiplier to compute true notional value.
    req.quanto_multiplier = m_orderFlow.quantoMultiplierFor(req.symbol);

    // M22 minimum-free-margin-after-stop gate: with a thin account a single
    // stop-loss can leave the account untradeable (2026-08-18: a -5.53
    // spike-through-SL stop on XAUUSD left the CFD account at 5.09 < the
    // 9.0 opening floor). When configured, a CFD order carrying an attached
    // SL must leave available - margin - |entry-sl|*qty*quanto >= the floor.
    if (MarketType::Cfd == req.market_type && req.sl_price.has_value()
        && m_cfg.risk.minAvailableAfterStopUsd.has_value()
        && m_cfg.risk.minAvailableAfterStopUsd.value() > 0.0
        && nullptr != m_cfdRest)
    {
        // Reference entry: the limit price when given, else the latest
        // CFD ticker (best effort for market orders).
        double ref_price = req.price;
        if (ref_price <= 0.0)
        {
            if (auto *feed = feedFor(MarketType::Cfd))
            {
                const auto tick = feed->tickerCache().get(req.symbol);
                if (tick.has_value())
                {
                    ref_price = tick->last;
                }
            }
        }
        double cfd_available = 0.0;
        {
            std::lock_guard lock(m_restMutex);
            auto cfd = m_cfdRest->getCfdAssets();
            if (ok(cfd))
            {
                const auto &data = value(cfd).value("data", nlohmann::json::object());
                cfd_available =
                    safeParseDouble(data.value("margin_free", "0")).value_or(0.0);
            }
        }
        if (ref_price > 0.0)
        {
            const double notional = ref_price * req.quantity
                                    * req.quanto_multiplier;
            // CFD manual orders don't carry a leverage field — fall back to
            // the configured instance leverage for this symbol. Assuming 1:1
            // (full notional) would block every 0.01-lot order (2026-08-18
            // round 152: XAUUSD 0.01 rejected with 4400 margin instead of
            // the exchange's ~8.8 at 500x).
            double leverage = req.leverage;
            if (leverage <= 0.0)
            {
                for (const auto &inst : m_cfg.strategy.strategies)
                {
                    if (inst.enabled && MarketType::Cfd == inst.market_type
                        && inst.symbol == req.symbol)
                    {
                        leverage = inst.leverage;
                        break;
                    }
                }
            }
            if (leverage <= 0.0)
            {
                leverage = 1.0;
            }
            const double margin = notional / leverage;
            const double stop_loss_usd =
                std::abs(ref_price - req.sl_price.value())
                * req.quantity * req.quanto_multiplier;
            const double free_after =
                cfd_available - margin - stop_loss_usd;
            const double floor = m_cfg.risk.minAvailableAfterStopUsd.value();
            if (free_after < floor)
            {
                return PulseError{
                    ErrorCode::InsufficientFreeMargin,
                    "open_order: CFD free margin after stop-loss ("
                        + std::to_string(free_after)
                        + " USD) would fall below "
                          "risk.minAvailableAfterStopUsd ("
                        + std::to_string(floor)
                        + "); available " + std::to_string(cfd_available)
                        + ", margin " + std::to_string(margin)
                        + ", stop-loss " + std::to_string(stop_loss_usd) };
            }
        }
    }

    // Manual path: futures/CFD are executable in any active direction (M22).
    // The "submitted" push event is emitted inside this call (see
    // publishOrderSubmitted): it must precede the tracker's immediate fill
    // poll, because an order that fills on submission completes synchronously
    // there and would otherwise publish `completed` before `submitted`.
    auto result = m_orderFlow.placeManualOrder(req);

    // Publish the outcome under the client key so a retry replays it instead
    // of re-sending the order. Three classes are distinguished:
    //   1. Success, or a definitive exchange rejection — cache verbatim.
    //   2. Transport failure that may have reached the exchange — cache an
    //      explicit "outcome unknown" marker. Replaying the raw timeout as a
    //      rejection would tell the caller "not placed", inviting a second
    //      order while the first one is live.
    //   3. Pre-flight failure (rate limit, halt, notional/position limits,
    //      missing executor) — abort, leaving the key retryable. Caching it
    //      would pin the key to a transient condition for the whole TTL.
    // Every earlier return between begin() and here happened before any order
    // could be sent and is covered by the guard's destructor.
    if (guard.has_value())
    {
        if (!ok(result) && orderMayHaveReachedExchange(error(result)))
        {
            PULSE_LOG_WARN("control",
                           "open_order: transport failure may have left order "
                           "{} live: {}",
                           req.client_order_id,
                           error(result).message);
            guard->complete(PulseError{
                ErrorCode::ControlOrderOutcomeUnknown,
                "open_order: the order may have reached the exchange before "
                "the request failed (" + error(result).message
                    + "); reconcile open orders by client_order_id before "
                      "resubmitting." });
        }
        else if (!ok(result) && isPreFlightRejection(error(result).code))
        {
            guard->abort();
        }
        else
        {
            guard->complete(result);
        }
    }

    return result;
}

Result<execution::OrderResponse>
EngineServices::closePosition(const nlohmann::json &params)
{
    if (!params.is_object())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "close_position: params must be an object" };
    }

    const auto position_id = params.value("position_id", "");
    if (position_id.empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "close_position: position_id is required" };
    }

    const auto pos_opt = m_positionMgr.getPosition(position_id);
    if (!pos_opt.has_value())
    {
        return PulseError{ ErrorCode::PositionLimitHit,
                           "close_position: position not found: " + position_id };
    }
    const auto &pos = *pos_opt;

    // TradFi CFD positions close via the dedicated close endpoint, not an
    // order: POST /tradfi/positions/{id}/close (close_type 2 = full,
    // 1 = partial with close_volume). The close does not emit an order fill,
    // so PositionManager is updated locally and the realized PnL feeds the
    // drawdown guard directly. To keep recentReports / trades.db /
    // execution.log complete, a terminal ExecutionReport is synthesized below
    // and routed through OrderFlowExecutor::recordCfdClose (2026-08-17
    // overnight finding: the close side previously left zero trace).
    if (MarketType::Cfd == pos.market_type)
    {
        if (nullptr == m_cfdRest)
        {
            return PulseError{ ErrorCode::InternalError,
                               "close_position: CFD infrastructure not configured" };
        }
        // The close endpoint needs the EXCHANGE position id (e.g. "17653462").
        // The internal position_id ("XAUUSD_Buy_1") is engine-local — the
        // exchange rejects it (400 "Data is being updated" is the generic
        // unknown-id response, verified 2026-08-17).
        const std::string exchange_pid = pos.exchange_position_id;
        if (exchange_pid.empty())
        {
            return PulseError{
                ErrorCode::InternalError,
                "close_position: position " + position_id
                    + " has no exchange position id (was it opened by this "
                      "engine? sync-imported CFD positions cannot be closed "
                      "by id)" };
        }
        const double close_qty = params.value("quantity", pos.quantity);
        const int close_type = (close_qty >= pos.quantity - 1e-9) ? 2 : 1;
        Result<nlohmann::json> res;
        {
            std::lock_guard lock(m_restMutex);
            res = m_cfdRest->postCfdPositionClose(exchange_pid, close_type, close_qty);
        }
        if (!ok(res))
        {
            return PulseError{ error(res).code,
                               "CFD close failed for " + position_id + ": "
                                   + error(res).message };
        }

        // A 2xx TradFi body can still be a business rejection
        // ({"label","message"}). Treating it as a fill would erase the local
        // position while the exchange still holds it — the gold trader's
        // stop would then silently disappear.
        if (auto business_error =
                execution::OrderExecutor::cfdBusinessError(value(res)))
        {
            return PulseError{ business_error->code,
                               "CFD close rejected for " + position_id + ": "
                                   + business_error->message };
        }
        // Local bookkeeping: exit at the latest known price (best effort).
        const double exit_price = pos.current_price > 0.0
                                      ? pos.current_price : pos.entry_price;
        const auto pnl = m_positionMgr.closePosition(
            position_id, close_qty, exit_price);
        const double realized = pnl.has_value() ? pnl.value() : 0.0;

        // Synthesize a terminal fill report for the close side and route it
        // through the shared record pipeline (completed reports, drawdown
        // guard, trades.db, execution.log). Without this, a CFD close leaves
        // zero trace anywhere.
        execution::ExecutionReport report;
        report.order_id            = "close_" + position_id;
        report.client_order_id     = report.order_id;
        report.symbol              = pos.symbol;
        report.side                = opposite(pos.side);
        report.type                = OrderType::Market;
        report.requested_qty       = close_qty;
        report.filled_qty          = close_qty;
        report.avg_fill_price      = exit_price;
        report.submit_mid_price    = exit_price;
        report.slippage_bps        = 0.0;
        report.fees                = 0.0;
        report.latency             = std::chrono::milliseconds{ 0 };
        report.submit_time         = now();
        report.fill_time           = now();
        report.final_status        = OrderStatus::Filled;
        report.exchange_position_id = exchange_pid;
        m_orderFlow.recordCfdClose(report, realized, pos.leverage);

        execution::OrderResponse resp;
        resp.order_id = position_id;
        resp.status = OrderStatus::Filled;
        resp.submit_time = now();
        return resp;
    }

    // Build a reduce/sell order through the full risk gate.
    execution::OrderRequest req;
    req.symbol        = pos.symbol;
    req.side          = opposite(pos.side);
    req.type          = OrderType::Market;
    req.market_type   = pos.market_type;
    req.quantity      = params.value("quantity", pos.quantity);
    req.leverage      = pos.leverage;
    req.reduce_only   = (MarketType::Futures == pos.market_type);
    req.client_order_id = "close_" + position_id;
    if (params.contains("price"))
    {
        req.type  = OrderType::Limit;
        req.price = params["price"].get<double>();
    }

    return m_orderFlow.placeOrder(req);
}

nlohmann::json EngineServices::switchDirection(const std::string &direction)
{
    const auto mt = parseMarketType(direction);
    nlohmann::json j;
    if (!mt.has_value())
    {
        j["error"] = "switch_direction: direction must be spot/futures/cfd";
        return j;
    }
    const MarketType target = *mt;

    // Infrastructure check — a direction that is not wired cannot activate.
    switch (target)
    {
    case MarketType::Cfd:
        if (nullptr == m_cfdRest || nullptr == m_cfdFeed || nullptr == m_cfdTracker)
        {
            j["error"] = "switch_direction: CFD not configured (add a "
                         "market_type=\"cfd\" strategy instance)";
            return j;
        }
        break;
    case MarketType::Futures:
        if (nullptr == m_futuresRest || nullptr == m_futuresTracker)
        {
            j["error"] = "switch_direction: futures not configured";
            return j;
        }
        break;
    default:
        if (nullptr == m_spotRest)
        {
            j["error"] = "switch_direction: spot not configured";
            return j;
        }
        break;
    }

    const MarketType old = m_orderFlow.activeMarket();
    if (old == target)
    {
        auto s = status();
        s["switched_from"] = toString(old);
        s["switched_to"] = toString(target);
        s["cancelled_orders"] = 0;
        return s;
    }

    // Serialize with all REST traffic: the gate closes first so no order for
    // the old direction can slip in during the pause/cancel sequence.
    std::lock_guard lock(m_restMutex);

    // 1. Close the gate for the new direction (atomic — rejects everything else).
    m_orderFlow.setActiveMarket(target);
    // 2. Strategies: pause the old direction ("strategies stopped"), resume the new one.
    const int paused = m_strategyMgr.setPausedByMarket(old, true);
    const int resumed = m_strategyMgr.setPausedByMarket(target, false);
    // 3. Cancel the old direction's open orders ("all resting orders cancelled").
    const int cancelled = m_orderFlow.cancelAllOpenOrders(old);

    auto s = status();
    s["switched_from"] = toString(old);
    s["switched_to"] = toString(target);
    s["strategies_paused"] = paused;
    s["strategies_resumed"] = resumed;
    s["cancelled_orders"] = cancelled;

    // M27: the switch may have swept the grid's futures orders — let the
    // grid mark them as externally cancelled (not fills) and re-hang per
    // the trend gate on its next reconcile.
    if (nullptr != m_grid)
    {
        m_grid->onDirectionSwitched();
    }
    return s;
}

// ---------------------------------------------------------------------------
// Futures trigger orders (price_orders) — M23
// ---------------------------------------------------------------------------
Result<nlohmann::json> EngineServices::placeTriggerOrder(const nlohmann::json &params)
{
    const auto contract = params.value("contract", "");
    if (contract.empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "place_trigger_order: contract is required" };
    }
    const double trigger_price = params.value("trigger_price", 0.0);
    if (trigger_price <= 0.0)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "place_trigger_order: trigger_price must be > 0" };
    }
    if (nullptr == m_futuresRest)
    {
        return PulseError{ ErrorCode::HttpError,
                           "place_trigger_order: futures REST client not wired" };
    }

    // rule: 1 = price ≥ trigger (SL direction for shorts / TP for longs),
    //       2 = price ≤ trigger (TP direction for shorts / SL for longs).
    // default 2 matches the grid-TP use case (short fills → TP below entry).
    const int rule = params.value("rule", 2);
    const int size = params.value("size", 0);
    const auto order_type =
        params.value("order_type", std::string("close-short-position"));
    const auto tif = params.value("tif", std::string("ioc"));
    const auto auto_size = params.value("auto_size", std::string("close"));

    // Gate price_orders closes the WHOLE position only (2026-08-18 live
    // probes): for close-*-position order types the exchange requires
    // auto_size non-empty (1017) and rejects non-zero size alongside it
    // (1014), and unknown auto_size enums are rejected (1021). Partial
    // closes (grid per-level TP) must use open_order with reduce_only.
    if (size > 0)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "place_trigger_order: Gate price_orders supports "
                           "whole-position closes only (size must be 0); for "
                           "partial closes use open_order with reduce_only" };
    }

    nlohmann::json body{
        { "initial", nlohmann::json{
              { "contract", contract },
              { "size", 0 },
              { "price", "0" },
              { "tif", tif },
              { "is_reduce_only", true },
              { "auto_size", auto_size },
          } },
        { "trigger", nlohmann::json{
              { "price", std::to_string(trigger_price) },
              { "rule", rule },
              { "price_type", params.value("price_type", 0) },
              { "expiration", 0 },
          } },
        { "order_type", order_type },
    };

    std::lock_guard lock(m_restMutex);
    return m_futuresRest->postFuturesPriceOrder(body);
}

Result<nlohmann::json> EngineServices::listFuturesOrders(const nlohmann::json &params)
{
    const auto contract = params.value("contract", "");
    if (contract.empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "list_futures_orders: contract is required" };
    }
    if (nullptr == m_futuresRest)
    {
        return PulseError{ ErrorCode::HttpError,
                           "list_futures_orders: futures REST client not wired" };
    }
    std::lock_guard lock(m_restMutex);
    return m_futuresRest->getFuturesOrders(contract);
}

double EngineServices::futuresPriceTick(const std::string &contract)
{
    // 1. Cached? (main-thread only — no lock on the map by contract)
    const auto cached = m_priceTicks.find(contract);
    if (m_priceTicks.end() != cached)
    {
        return cached->second;
    }

    if (nullptr == m_futuresRest)
    {
        return 0.0;
    }

    // 2. Fetch the contract list under the shared REST mutex and pick the
    //    contract's order_price_round. Gate rejects trigger prices that are
    //    not an integer multiple of this unit (1009, testnet 2026-11-13).
    Result<nlohmann::json> contracts;
    {
        std::lock_guard lock(m_restMutex);
        contracts = m_futuresRest->getFuturesContracts();
    }
    if (!ok(contracts))
    {
        PULSE_LOG_WARN("control", "futuresPriceTick: contract list failed: {}",
                       error(contracts).message);
        return 0.0;
    }

    for (const auto &c : value(contracts))
    {
        if (contract != c.value("name", ""))
        {
            continue;
        }
        const double tick =
            safeParseDouble(c.value("order_price_round", "0")).value_or(0.0);
        m_priceTicks[contract] = tick;
        return tick;
    }

    return 0.0;
}

int EngineServices::ensureFuturesStops()
{
    // 1. Feature gate: opt-in and requires a futures REST client.
    if (!m_cfg.risk.exchange_stop.enabled || nullptr == m_futuresRest)
    {
        return 0;
    }

    const auto positions = m_positionMgr.getAllPositions();
    std::unordered_set<std::string> live;
    int placed = 0;

    for (const auto &pos : positions)
    {
        if (MarketType::Futures != pos.market_type)
        {
            continue; // Spot and CFD have their own protection paths.
        }
        live.insert(pos.position_id);

        // 2. Already handled in a previous call?
        if (0 != m_exchangeStopPositions.count(pos.position_id))
        {
            continue;
        }

        // 3. Covered by an existing open trigger on the contract? Gate's
        //    close-position trigger is whole-position, so one per contract
        //    is enough.
        auto triggers = listTriggerOrders(nlohmann::json{ { "contract", pos.symbol } });
        if (!ok(triggers))
        {
            PULSE_LOG_WARN("control",
                           "ensureFuturesStops: list triggers for {} failed: {}",
                           pos.symbol, error(triggers).message);
            continue;
        }
        if (!value(triggers).empty())
        {
            m_exchangeStopPositions.insert(pos.position_id);
            continue;
        }

        // 4. Plan the stop: round to the contract tick and refuse a stop the
        //    market has already gapped through.
        const double tick = futuresPriceTick(pos.symbol);
        double last = 0.0;
        if (nullptr != m_futuresFeed)
        {
            const auto quote = m_futuresFeed->tickerCache().get(pos.symbol);
            if (quote.has_value())
            {
                last = quote->last;
            }
        }

        const auto plan = execution::planExchangeStop(
            pos.side, pos.entry_price, m_cfg.risk.exchange_stop.pct, tick, last);
        if (!plan.has_value())
        {
            PULSE_LOG_WARN("control",
                           "ensureFuturesStops: no valid stop for {} "
                           "(entry={:.2f} tick={:.4f} last={:.2f})",
                           pos.position_id, pos.entry_price, tick, last);
            continue;
        }

        // 5. Place the whole-position reduce-only trigger.
        nlohmann::json params{
            { "contract", pos.symbol },
            { "trigger_price", plan->trigger_price },
            { "rule", plan->rule },
            { "order_type", plan->order_type },
            { "size", 0 },
            { "auto_size", "close" },
        };

        auto result = placeTriggerOrder(params);
        if (!ok(result))
        {
            PULSE_LOG_ERROR("control",
                            "ensureFuturesStops: placement failed for {}: {}",
                            pos.position_id, error(result).message);
            continue; // Retried on the next call.
        }

        m_exchangeStopPositions.insert(pos.position_id);
        ++placed;
        PULSE_LOG_INFO("control",
                       "Exchange stop placed for {} at {:.2f} (entry {:.2f})",
                       pos.position_id, plan->trigger_price, pos.entry_price);
    }

    // 6. Forget positions that are no longer open.
    for (auto it = m_exchangeStopPositions.begin();
         it != m_exchangeStopPositions.end();)
    {
        if (0 == live.count(*it))
        {
            it = m_exchangeStopPositions.erase(it);
        }
        else
        {
            ++it;
        }
    }

    return placed;
}

Result<nlohmann::json> EngineServices::listTriggerOrders(const nlohmann::json &params)
{
    const auto contract = params.value("contract", "");
    if (contract.empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "list_trigger_orders: contract is required" };
    }
    if (nullptr == m_futuresRest)
    {
        return PulseError{ ErrorCode::HttpError,
                           "list_trigger_orders: futures REST client not wired" };
    }
    std::lock_guard lock(m_restMutex);
    return m_futuresRest->getFuturesPriceOrders(contract);
}

Result<nlohmann::json> EngineServices::cancelTriggerOrder(const nlohmann::json &params)
{
    const auto order_id = params.value("order_id", "");
    if (order_id.empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "cancel_trigger_order: order_id is required" };
    }
    if (nullptr == m_futuresRest)
    {
        return PulseError{ ErrorCode::HttpError,
                           "cancel_trigger_order: futures REST client not wired" };
    }
    std::lock_guard lock(m_restMutex);
    return m_futuresRest->cancelFuturesPriceOrder(order_id);
}

bool EngineServices::cancelOrder(const std::string &order_id)
{
    if (order_id.empty())
    {
        return false;
    }

    std::lock_guard lock(m_restMutex);
    return m_orderFlow.cancelOrder(order_id);
}

Result<nlohmann::json> EngineServices::gridStart(
    const nlohmann::json &params)
{
    if (nullptr == m_grid)
    {
        return PulseError{ ErrorCode::GridNotStarted,
                           "grid service not configured in this build" };
    }
    const auto result = m_grid->start(params);
    if (!ok(result))
    {
        return error(result); // 9201 already-running / 9101 validation, as-is
    }
    return pulse::grid::gridSnapshotToJson(value(result));
}

nlohmann::json EngineServices::gridStatus() const
{
    if (nullptr == m_grid)
    {
        return nlohmann::json{ { "phase", 0 },
                               { "phase_name", "disabled" },
                               { "error", "grid service not configured" } };
    }
    return pulse::grid::gridSnapshotToJson(m_grid->status());
}

Result<nlohmann::json> EngineServices::gridPause()
{
    if (nullptr == m_grid)
    {
        return PulseError{ ErrorCode::GridNotStarted,
                           "grid service not configured in this build" };
    }
    const auto result = m_grid->pause();
    if (!ok(result))
    {
        return PulseError{ ErrorCode::GridNotStarted,
                           error(result).message };
    }
    return pulse::grid::gridSnapshotToJson(value(result));
}

Result<nlohmann::json> EngineServices::gridStop()
{
    if (nullptr == m_grid)
    {
        return PulseError{ ErrorCode::GridNotStarted,
                           "grid service not configured in this build" };
    }
    const auto result = m_grid->stop();
    if (!ok(result))
    {
        return PulseError{ ErrorCode::GridNotStarted,
                           error(result).message };
    }
    return pulse::grid::gridSnapshotToJson(value(result));
}

Result<nlohmann::json> EngineServices::gridResume()
{
    if (nullptr == m_grid)
    {
        return PulseError{ ErrorCode::GridNotStarted,
                           "grid service not configured in this build" };
    }
    const auto result = m_grid->resume();
    if (!ok(result))
    {
        return PulseError{ ErrorCode::GridNotPaused, error(result).message };
    }
    return pulse::grid::gridSnapshotToJson(value(result));
}

void EngineServices::haltTrading()
{
    m_riskMgr.drawdownGuard().manualHalt();
}

void EngineServices::resumeTrading()
{
    m_riskMgr.drawdownGuard().clearHalt();
}

bool EngineServices::pauseStrategy(const std::string &id)
{
    return m_strategyMgr.setPaused(id, true);
}

bool EngineServices::resumeStrategy(const std::string &id)
{
    return m_strategyMgr.setPaused(id, false);
}

bool EngineServices::externalOrdersEnabled() const
{
    return m_externalOrdersEnabled.load(std::memory_order_relaxed);
}

void EngineServices::setExternalOrdersEnabled(bool enabled)
{
    m_externalOrdersEnabled.store(enabled, std::memory_order_relaxed);
    // Audit trail: arming or killing the external runtime is an operationally
    // significant event, so leave a line in the engine log even though the
    // JSON reply already reports the new state.
    PULSE_LOG_INFO("control", "external order gate {}",
                   enabled ? "ENABLED" : "DISABLED");
}

bool EngineServices::setStrategyTrading(const std::string &id, bool enabled)
{
    // The execution gate is a StrategyParams field, so the regular param
    // path gives us bounds clamping + audit logging for free. Unknown
    // strategy → false (setStrategyParam already returns false).
    return setStrategyParam(id, "auto_trade", enabled ? 1.0 : 0.0);
}

std::mutex &EngineServices::restMutex()
{
    return m_restMutex;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
nlohmann::json EngineServices::strategyParamsJson(
    const strategy::StrategyParams &p) const
{
    nlohmann::json j;
    for (const auto &[name, getter] : paramGetters())
    {
        j[name] = getter(p);
    }
    return j;
}

// ---------------------------------------------------------------------------
// Position sync — startup, ~10s background tick, manual `sync_positions`.
// Consolidated from the old main.cpp free functions so all three paths share
// one implementation (and one pruning rule for manual app-side closes).
// ---------------------------------------------------------------------------

/// Read a numeric field that may arrive as a JSON string or number.
static double syncJsonNumber(const nlohmann::json &j, const char *key)
{
    const auto it = j.find(key);
    if (j.end() == it)
    {
        return 0.0;
    }
    if (it->is_string())
    {
        return safeParseDouble(it->get<std::string>()).value_or(0.0);
    }
    if (it->is_number())
    {
        return it->get<double>();
    }
    return 0.0;
}

int EngineServices::syncFuturesPositionsFromExchange()
{
    if (nullptr == m_futuresRest)
    {
        return 0;
    }

    nlohmann::json positions;
    {
        std::lock_guard<std::mutex> rest_lock(m_restMutex);
        auto result = m_futuresRest->getFuturesPositions();
        if (!ok(result))
        {
            PULSE_LOG_WARN("app", "Position sync (futures) skipped: {}",
                           error(result).message);
            return 0;
        }
        positions = value(result);
    }

    int synced = 0;
    std::set<std::string> live_contracts; // contracts the exchange still holds
    for (const auto &p : positions)
    {
        const std::string contract = p.value("contract", "");
        if (contract.empty())
        {
            continue;
        }
        const int size = p.value("size", 0);
        if (0 == size)
        {
            continue;
        }
        live_contracts.insert(contract);

        const double entry = syncJsonNumber(p, "entry_price");
        const double mark = syncJsonNumber(p, "mark_price");

        // Gate reports leverage = 0 (as a STRING) for cross margin; fall
        // back to the account's cross leverage limit for margin/PnL math.
        double leverage = syncJsonNumber(p, "leverage");
        if (leverage <= 0.0)
        {
            leverage = syncJsonNumber(p, "cross_leverage_limit");
        }
        if (leverage <= 0.0)
        {
            leverage = 10.0;
        }

        double quanto_multiplier = 1.0;
        if (m_registry)
        {
            if (const auto info = m_registry->get(contract))
            {
                quanto_multiplier = info->quanto_multiplier;
            }
        }

        // Exchange open_time is a unix SECONDS string; Position stores ns.
        const double open_secs = syncJsonNumber(p, "open_time");
        Timestamp open_time{};
        if (open_secs > 0.0)
        {
            open_time = std::chrono::time_point_cast<Duration>(
                std::chrono::system_clock::time_point{
                    std::chrono::seconds{static_cast<std::int64_t>(open_secs)}});
        }

        const bool is_long = size > 0;
        m_positionMgr.syncPositionFromExchange(
            contract, is_long ? Side::Buy : Side::Sell,
            static_cast<double>(std::abs(size)), entry, mark,
            MarketType::Futures, leverage, MarginMode::Cross,
            quanto_multiplier, syncJsonNumber(p, "maintenance_rate"),
            syncJsonNumber(p, "liq_price"), open_time);
        ++synced;
    }
    PULSE_LOG_INFO("app",
        "Position sync (futures): {} position(s) imported from exchange",
        synced);
    // Prune fill-tracked ghosts on contracts the exchange no longer holds
    // (external app close / liquidation) — the count is logged inside.
    (void)pruneGhostFuturesByContract(live_contracts);
    return synced;
}

int EngineServices::syncCfdPositionsFromExchange(int *pruned_out)
{
    if (nullptr == m_cfdRest)
    {
        if (pruned_out)
        {
            *pruned_out = 0;
        }
        return 0;
    }

    nlohmann::json data;
    {
        std::lock_guard<std::mutex> rest_lock(m_restMutex);
        auto result = m_cfdRest->getCfdPositions();
        if (!ok(result))
        {
            PULSE_LOG_WARN("app", "Position sync (CFD) skipped: {}",
                           error(result).message);
            if (pruned_out)
            {
                *pruned_out = 0;
            }
            return 0;
        }
        const auto &resp = value(result);
        data = resp.value("data", nlohmann::json::object());
    }

    const auto &list = data.value("list", nlohmann::json::array());
    std::vector<std::string> live_exchange_ids;
    int synced = 0;

    if (list.is_array())
    {
        for (const auto &p : list)
        {
            const std::string symbol = p.value("symbol", "");
            if (symbol.empty())
            {
                continue;
            }
            const bool is_long = ("Long" == p.value("position_dir", ""));
            const double volume = syncJsonNumber(p, "volume");
            if (volume <= 0.0)
            {
                continue;
            }

            // The close/modify endpoints need the EXCHANGE position id.
            const std::string exchange_id = std::to_string(
                static_cast<std::int64_t>(syncJsonNumber(p, "position_id")));
            live_exchange_ids.push_back(exchange_id);

            const double entry = syncJsonNumber(p, "price_open");
            const double mark = syncJsonNumber(p, "counterparty_price");
            double leverage = syncJsonNumber(p, "leverage");
            if (leverage <= 0.0)
            {
                leverage = 1.0;
            }

            // Exchange time_create is unix SECONDS; Position stores ns.
            const double create_secs = syncJsonNumber(p, "time_create");
            Timestamp open_time{};
            if (create_secs > 0.0)
            {
                open_time = std::chrono::time_point_cast<Duration>(
                    std::chrono::system_clock::time_point{
                        std::chrono::seconds{
                            static_cast<std::int64_t>(create_secs)}});
            }

            // CFD symbols carry their own contract-size multiplier in the
            // symbol registry (quanto=100 for XAUUSD = 1 lot of 100 oz).
            // Use the same multiplier as the fill path so notional/PnL
            // conventions agree between engine-opened and synced positions.
            const double quanto = m_orderFlow.quantoMultiplierFor(symbol);
            m_positionMgr.syncPositionFromExchange(
                symbol, is_long ? Side::Buy : Side::Sell, volume, entry, mark,
                MarketType::Cfd, leverage, MarginMode::Cross, quanto, 0.0,
                syncJsonNumber(p, "liq_price"), open_time,
                syncJsonNumber(p, "price_sl"), syncJsonNumber(p, "price_tp"),
                exchange_id);

            PULSE_LOG_INFO("app",
                "Position sync (CFD): {} {} {} lots @ {} (mark {}, sl {}, tp {})",
                symbol, (is_long ? "long" : "short"), volume, entry, mark,
                syncJsonNumber(p, "price_sl"), syncJsonNumber(p, "price_tp"));
            ++synced;
        }
    }

    // Prune local ghosts: the user frequently closes CFD positions manually
    // in the app; the engine only learns about it from the exchange list.
    const int pruned = pruneGhostPositions(MarketType::Cfd, live_exchange_ids);
    if (pruned_out)
    {
        *pruned_out = pruned;
    }
    return synced;
}

int EngineServices::pruneGhostPositions(
    MarketType mt, const std::vector<std::string> &live_exchange_ids)
{
    // Grace window: a freshly-opened engine position may not have appeared
    // in the exchange list yet (fill latency, list pagination). Only prune
    // positions older than the grace period so we never drop a real one.
    constexpr auto kGrace = std::chrono::seconds{ 60 };
    const auto cutoff = now() - kGrace;

    int pruned = 0;
    for (const auto &pos : m_positionMgr.getAllPositions())
    {
        if (mt != pos.market_type)
        {
            continue;
        }
        if (pos.exchange_position_id.empty())
        {
            // No exchange id to compare — leave it to the restart sync.
            continue;
        }
        const bool still_live = std::find(live_exchange_ids.begin(),
                                          live_exchange_ids.end(),
                                          pos.exchange_position_id)
            != live_exchange_ids.end();
        if (still_live)
        {
            continue;
        }
        if (pos.open_time > cutoff)
        {
            continue; // Too fresh — likely a fill that has not appeared yet.
        }
        if (pos.quantity > 0.0)
        {
            pruned += traceExternalGhostClose(pos);
        }
        else if (m_positionMgr.removePosition(pos.position_id))
        {
            PULSE_LOG_INFO("app",
                "Position sync: pruned ghost {} (absent from exchange)",
                pos.position_id);
            ++pruned;
        }
    }
    if (pruned > 0)
    {
        PULSE_LOG_INFO("app", "Position sync: pruned {} ghost position(s)", pruned);
    }
    return pruned;
}

int EngineServices::traceExternalGhostClose(const risk::Position &pos)
{
    // The position vanished from the exchange with no local close record —
    // the user closed it manually in the app, or an exchange-side stop /
    // liquidation fired without the engine ever seeing the order. Trace the
    // close side through the shared record pipeline (recentReports /
    // trades.db / drawdown guard / execution.log) so the audit trail is
    // complete. The exit price is a best-effort estimate from the last known
    // mark (falling back to entry → realized 0, keeping the ledger clean) —
    // order_id carries the "ext_" marker and the log flags the estimate so
    // consumers can reconcile via API.
    const double exit_price = pos.current_price > 0.0
                                  ? pos.current_price : pos.entry_price;
    const auto pnl = m_positionMgr.closePosition(
        pos.position_id, pos.quantity, exit_price);
    const double realized = pnl.has_value() ? pnl.value() : 0.0;

    execution::ExecutionReport report;
    const std::string ref = pos.exchange_position_id.empty()
                                ? pos.position_id
                                : pos.exchange_position_id;
    report.order_id          = "ext_close_" + ref;
    report.client_order_id   = report.order_id;
    report.symbol            = pos.symbol;
    report.side              = opposite(pos.side);
    report.type              = OrderType::Market;
    report.requested_qty     = pos.quantity;
    report.filled_qty        = pos.quantity;
    report.avg_fill_price    = exit_price;
    report.submit_mid_price  = exit_price;
    report.slippage_bps      = 0.0;
    report.fees              = 0.0;
    report.latency           = std::chrono::milliseconds{ 0 };
    report.submit_time       = now();
    report.fill_time         = now();
    report.final_status      = OrderStatus::Filled;
    report.exchange_position_id = pos.exchange_position_id;
    m_orderFlow.recordExternalClose(report, realized, pos.market_type,
                                    pos.leverage);

    PULSE_LOG_INFO("app",
        "Position sync: pruned ghost {} (absent from exchange; "
        "external close traced, exit ~{:.2f}, pnl {:.4f})",
        pos.position_id, exit_price, realized);
    return 1;
}

int EngineServices::pruneGhostFuturesByContract(
    const std::set<std::string> &live_contracts)
{
    // Grace window: a freshly-opened engine fill may not have appeared in
    // the exchange positions list yet — only prune fills older than the
    // grace period so we never drop a real position.
    constexpr auto kGrace = std::chrono::seconds{ 60 };
    const auto cutoff = now() - kGrace;

    int pruned = 0;
    for (const auto &pos : m_positionMgr.getAllPositions())
    {
        if (MarketType::Futures != pos.market_type)
        {
            continue;
        }
        if (pos.quantity <= 0.0)
        {
            continue;
        }
        if (live_contracts.contains(pos.symbol))
        {
            continue; // contract still held on the exchange
        }
        if (pos.open_time > cutoff)
        {
            continue; // too fresh — likely a fill that has not appeared yet.
        }
        pruned += traceExternalGhostClose(pos);
    }
    if (pruned > 0)
    {
        PULSE_LOG_INFO("app",
            "Position sync: pruned {} ghost futures position(s)", pruned);
    }
    return pruned;
}

nlohmann::json EngineServices::syncPositions()
{
    nlohmann::json summary{
        { "futures_synced", 0 },
        { "cfd_synced", 0 },
        { "pruned", 0 },
        { "error", nullptr },
    };

    if (m_futuresRest)
    {
        summary["futures_synced"] = syncFuturesPositionsFromExchange();
    }
    if (m_cfdRest)
    {
        int pruned = 0;
        summary["cfd_synced"] = syncCfdPositionsFromExchange(&pruned);
        summary["pruned"] = pruned;
    }

    return summary;
}

Result<nlohmann::json> EngineServices::modifySlTp(const nlohmann::json &params)
{
    if (!params.is_object())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "modify_sl_tp: params must be an object" };
    }

    const auto position_id = params.value("position_id", "");
    if (position_id.empty())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "modify_sl_tp: position_id is required" };
    }

    const bool has_sl = params.contains("sl_price")
        && params["sl_price"].is_number();
    const bool has_tp = params.contains("tp_price")
        && params["tp_price"].is_number();
    if (!has_sl && !has_tp)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "modify_sl_tp: at least one of sl_price/tp_price "
                           "is required" };
    }

    const auto pos_opt = m_positionMgr.getPosition(position_id);
    if (!pos_opt.has_value())
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "modify_sl_tp: position not found: " + position_id };
    }
    const auto &pos = *pos_opt;
    if (MarketType::Cfd != pos.market_type)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "modify_sl_tp: only CFD positions support "
                           "attached SL/TP" };
    }

    if (nullptr == m_cfdRest)
    {
        return PulseError{ ErrorCode::InternalError,
                           "modify_sl_tp: CFD infrastructure not configured" };
    }

    // The modify endpoint addresses the EXCHANGE position id, exactly like
    // the close endpoint (close-id translation verified live on 2026-08-17).
    // Passing the engine-local id ("XAUUSD_Buy_1") targets an id Gate never
    // issued; this path previously did so. Every CFD position carries the
    // exchange id after a fill or a sync, so a missing one means there is
    // nothing the exchange could address.
    if (pos.exchange_position_id.empty())
    {
        return PulseError{ ErrorCode::InternalError,
                           "modify_sl_tp: position " + position_id
                               + " has no exchange position id" };
    }

    // Omitted fields keep the currently attached value ("0" = no stop). The
    // engine's copy can be up to one position-sync interval stale, and the
    // PUT rewrites BOTH prices, so read the live position whenever a field is
    // omitted. Otherwise a stop tightening could silently revert (or clear)
    // a take-profit the user just changed in the Gate app.
    double sl = has_sl ? params["sl_price"].get<double>() : pos.sl_price;
    double tp = has_tp ? params["tp_price"].get<double>() : pos.tp_price;

    std::lock_guard<std::mutex> rest_lock(m_restMutex);

    if (!has_sl || !has_tp)
    {
        auto live = m_cfdRest->getCfdPositions();
        if (!ok(live))
        {
            return PulseError{ error(live).code,
                               "modify_sl_tp: cannot read live protective "
                               "prices for "
                                   + position_id + ": " + error(live).message };
        }

        const auto &data = value(live).value("data", nlohmann::json::object());
        const auto &list = data.value("list", nlohmann::json::array());
        bool found = false;

        if (list.is_array())
        {
            for (const auto &entry : list)
            {
                const std::string exchange_id = std::to_string(
                    static_cast<std::int64_t>(syncJsonNumber(entry, "position_id")));
                if (exchange_id != pos.exchange_position_id)
                {
                    continue;
                }
                sl = has_sl ? sl : syncJsonNumber(entry, "price_sl");
                tp = has_tp ? tp : syncJsonNumber(entry, "price_tp");
                found = true;
                break;
            }
        }

        if (!found)
        {
            return PulseError{ ErrorCode::InternalError,
                               "modify_sl_tp: position " + position_id
                                   + " is no longer on the exchange" };
        }
    }

    auto result = m_cfdRest->putCfdPositionModify(
        pos.exchange_position_id, std::to_string(sl), std::to_string(tp));
    if (!ok(result))
    {
        return error(result);
    }

    // TradFi reports business errors as a 2xx body {"label","message"}; the
    // HTTP status alone would record a rejected stop as accepted.
    if (auto business_error =
            execution::OrderExecutor::cfdBusinessError(value(result)))
    {
        return *business_error;
    }

    // Refresh the local view immediately (the periodic sync would catch up
    // within ~10s anyway).
    m_positionMgr.updateExchangeStops(position_id, sl, tp);
    PULSE_LOG_INFO("app", "modify_sl_tp: {} -> sl {}, tp {}",
                   position_id, sl, tp);

    return value(result);
}

} // namespace pulse::control
