// EngineServices.cpp — see EngineServices.hpp

#include "control/EngineServices.hpp"

#include "control/OrderFlowExecutor.hpp"
#include "core/snapshot_types.hpp"
#include "core/types.hpp"
#include "risk/DrawdownGuard.hpp"

#include <chrono>
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
    exchange::GateRestClient *spot_rest,
    exchange::GateRestClient *futures_rest,
    execution::OrderTracker *spot_tracker,
    execution::OrderTracker *futures_tracker,
    OrderFlowExecutor &order_flow,
    std::mutex &rest_mutex)
    : m_version{ std::move(version) }
    , m_engineStart{ engine_start }
    , m_cfg{ cfg }
    , m_strategyMgr{ strategy_mgr }
    , m_riskMgr{ risk_mgr }
    , m_positionMgr{ position_mgr }
    , m_spotFeed{ spot_feed }
    , m_futuresFeed{ futures_feed }
    , m_spotRest{ spot_rest }
    , m_futuresRest{ futures_rest }
    , m_spotTracker{ spot_tracker }
    , m_futuresTracker{ futures_tracker }
    , m_orderFlow{ order_flow }
    , m_restMutex{ rest_mutex }
{
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

    if (m_spotFeed)
    {
        j["feed_spot"] = m_spotFeed->stats();
    }
    if (m_futuresFeed)
    {
        j["feed_futures"] = m_futuresFeed->stats();
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

    return j;
}

nlohmann::json EngineServices::positions() const
{
    nlohmann::json j;
    j["positions"] = m_positionMgr.getAllPositions();
    j["portfolio"] = m_positionMgr.portfolioSummary();
    return j;
}

nlohmann::json EngineServices::orders() const
{
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
    j["activeOrders"] = active;
    j["recentReports"] = reports;
    return j;
}

nlohmann::json EngineServices::strategies() const
{
    return m_strategyMgr.snapshot();
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
    it->second(*params, value);
    return true;
}

nlohmann::json EngineServices::risk() const
{
    return m_riskMgr.riskSnapshot();
}

nlohmann::json EngineServices::market(const std::string &symbol,
                                      int book_levels,
                                      int klines) const
{
    nlohmann::json j;
    j["symbol"] = symbol;

    // Pick feed: futures by default for futures strategies, else spot.
    auto *feed = m_futuresFeed ? m_futuresFeed : m_spotFeed;
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
                               "open_order: market_type must be spot/futures" };
        }
        req.market_type = *mt;
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

    return m_orderFlow.placeOrder(req);
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

bool EngineServices::cancelOrder(const std::string &order_id)
{
    if (order_id.empty())
    {
        return false;
    }

    std::lock_guard lock(m_restMutex);
    return m_orderFlow.cancelOrder(order_id);
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

} // namespace pulse::control
