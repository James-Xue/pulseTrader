// EngineServices.cpp — see EngineServices.hpp

#include "control/EngineServices.hpp"

#include "control/OrderFlowExecutor.hpp"
#include "grid/GridManager.hpp"
#include "ai/ParamBounds.hpp"
#include "core/snapshot_types.hpp"
#include "core/types.hpp"
#include "execution/ExecutionReport.hpp"
#include "logging/Logger.hpp"
#include "risk/DrawdownGuard.hpp"

#ifdef PULSE_ENABLE_SQLITE
#include "trade_recorder/TradeRecorder.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace pulse::control
{

namespace
{

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

} // anonymous namespace

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
    j["symbols"] = m_cfg.symbols;
    j["strategies_running"] = m_strategyMgr.runningCount();
    j["strategies_total"] = m_strategyMgr.strategyCount();
    j["open_positions"] = m_positionMgr.openPositionCount();
    j["trading_halted"] = m_riskMgr.isTradingHalted();
    j["active_market"] = toString(m_orderFlow.activeMarket());

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

nlohmann::json EngineServices::account()
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
            j["spot"] = spotUsdtAccount(value(spot));
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

nlohmann::json EngineServices::positions() const
{
    // Reconcile first so fills that missed the WS private channel (market
    // orders fill instantly, before the subscription exists) surface as
    // positions before the snapshot is taken.
    if (m_spotTracker)
    {
        m_spotTracker->reconcileAll();
    }
    if (m_futuresTracker)
    {
        m_futuresTracker->reconcileAll();
    }
    if (m_cfdTracker)
    {
        m_cfdTracker->reconcileAll();
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

nlohmann::json EngineServices::orders() const
{
    // Same reconcile as positions(): present tracked orders' real state
    // instead of a stale Pending from before the WS subscription existed.
    if (m_spotTracker)
    {
        m_spotTracker->reconcileAll();
    }
    if (m_futuresTracker)
    {
        m_futuresTracker->reconcileAll();
    }
    if (m_cfdTracker)
    {
        m_cfdTracker->reconcileAll();
    }
    nlohmann::json j;
    std::vector<execution::OrderSnapshot> active;
    std::vector<execution::ExecutionReport> reports;
    if (m_spotTracker)
    {
        auto a = m_spotTracker->activeOrders();
        active.insert(active.end(), a.begin(), a.end());
        auto r = m_spotTracker->recentReports(20);
        reports.insert(reports.end(), r.begin(), r.end());
    }
    if (m_futuresTracker)
    {
        auto a = m_futuresTracker->activeOrders();
        active.insert(active.end(), a.begin(), a.end());
        auto r = m_futuresTracker->recentReports(20);
        reports.insert(reports.end(), r.begin(), r.end());
    }
    if (m_cfdTracker)
    {
        auto a = m_cfdTracker->activeOrders();
        active.insert(active.end(), a.begin(), a.end());
        auto r = m_cfdTracker->recentReports(20);
        reports.insert(reports.end(), r.begin(), r.end());
    }
    j["activeOrders"] = active;
    j["recentReports"] = reports;

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
    if (params.contains("client_order_id"))
    {
        req.client_order_id = params["client_order_id"].get<std::string>();
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
    return m_orderFlow.placeManualOrder(req);
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
    // 2. Strategies: pause the old direction ("策略停跑"), resume the new one.
    const int paused = m_strategyMgr.setPausedByMarket(old, true);
    const int resumed = m_strategyMgr.setPausedByMarket(target, false);
    // 3. Cancel the old direction's open orders ("挂单全撤").
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

    // Omitted fields keep the currently attached value ("0" = no stop).
    const double sl = has_sl ? params["sl_price"].get<double>()
                             : pos.sl_price;
    const double tp = has_tp ? params["tp_price"].get<double>()
                             : pos.tp_price;

    std::lock_guard<std::mutex> rest_lock(m_restMutex);
    auto result = m_cfdRest->putCfdPositionModify(
        position_id, std::to_string(sl), std::to_string(tp));
    if (!ok(result))
    {
        return error(result);
    }

    // Refresh the local view immediately (the periodic sync would catch up
    // within ~10s anyway).
    m_positionMgr.updateExchangeStops(position_id, sl, tp);
    PULSE_LOG_INFO("app", "modify_sl_tp: {} -> sl {}, tp {}",
                   position_id, sl, tp);

    return value(result);
}

} // namespace pulse::control
