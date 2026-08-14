// OrderFlowExecutor.cpp — see OrderFlowExecutor.hpp for the flow description

#include "control/OrderFlowExecutor.hpp"

#include "core/types.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <cmath>

namespace pulse::control
{

// ---------------------------------------------------------------------------
// ExecutorOrderPlacer
// ---------------------------------------------------------------------------
Result<execution::OrderResponse>
ExecutorOrderPlacer::place(const execution::OrderRequest &req)
{
    return m_exec.placeOrder(req);
}

bool ExecutorOrderPlacer::cancel(const std::string &order_id)
{
    return m_exec.cancelOrder(order_id);
}

Result<nlohmann::json> ExecutorOrderPlacer::setLeverage(const std::string &contract,
                                                        double leverage)
{
    return m_exec.setLeverage(contract, leverage);
}

// ---------------------------------------------------------------------------
// OrderFlowExecutor
// ---------------------------------------------------------------------------
OrderFlowExecutor::OrderFlowExecutor(
    const StrategyConfig &strategy_cfg,
    risk::RiskManager &risk_mgr,
    risk::PositionManager &position_mgr,
    risk::DrawdownGuard &drawdown_guard,
    IOrderPlacer *spot_placer,
    IOrderPlacer *futures_placer,
    execution::OrderTracker *spot_tracker,
    execution::OrderTracker *futures_tracker,
    std::mutex &rest_mutex,
#ifdef PULSE_ENABLE_SQLITE
    trade_recorder::TradeRecorder *trade_recorder
#else
    int
#endif
)
    : m_strategyCfg{ strategy_cfg }
    , m_riskMgr{ risk_mgr }
    , m_positionMgr{ position_mgr }
    , m_drawdownGuard{ drawdown_guard }
    , m_spotPlacer{ spot_placer }
    , m_futuresPlacer{ futures_placer }
    , m_spotTracker{ spot_tracker }
    , m_futuresTracker{ futures_tracker }
    , m_restMutex{ rest_mutex }
#ifdef PULSE_ENABLE_SQLITE
    , m_tradeRecorder{ trade_recorder }
#endif
{
    // Wire completion callbacks — the class owns the completion logic.
    if (m_spotTracker)
    {
        m_spotTracker->setCompletionCallback(
            [this](const execution::ExecutionReport &report)
            { onOrderComplete(report); });
    }
    if (m_futuresTracker)
    {
        m_futuresTracker->setCompletionCallback(
            [this](const execution::ExecutionReport &report)
            { onOrderComplete(report); });
    }
}

execution::OrderRequest
OrderFlowExecutor::buildRequestFromSignal(const strategy::TradingSignal &sig) const
{
    execution::OrderRequest req;
    req.symbol      = sig.symbol;
    req.side        = (strategy::SignalType::Buy == sig.type) ? Side::Buy : Side::Sell;
    req.type        = OrderType::Market;
    req.price       = sig.price;
    req.market_type = sig.market_type;

    // Find strategy config for leverage/quantity settings.
    // Signal aggregator uses strategy_id "signal_aggregator" which won't
    // match any instance name — fall back to first strategy on the same symbol.
    req.quantity = 0.001;
    bool matched = false;
    for (const auto &inst : m_strategyCfg.strategies)
    {
        if (inst.name == sig.strategy_id)
        {
            req.quantity     = inst.order_quantity;
            req.market_type  = inst.market_type;
            req.leverage     = inst.leverage;
            matched          = true;
            break;
        }
    }
    if (!matched)
    {
        for (const auto &inst : m_strategyCfg.strategies)
        {
            if (inst.enabled && inst.symbol == sig.symbol)
            {
                req.quantity    = inst.order_quantity;
                req.market_type = inst.market_type;
                req.leverage    = inst.leverage;
                break;
            }
        }
    }

    // For futures: convert quantity to contract_size (integer contracts).
    if (MarketType::Futures == req.market_type && 0 == req.contract_size)
    {
        req.contract_size = static_cast<int>(std::max(1.0, std::round(req.quantity)));
    }

    return req;
}

void OrderFlowExecutor::onSignal(const strategy::TradingSignal &sig)
{
    auto log_app = logging::Logger::get("app");

    // Skip Flat signals.
    if (strategy::SignalType::Flat == sig.type)
    {
        return;
    }

    auto req = buildRequestFromSignal(sig);

    // Risk evaluation.
    auto eval = m_riskMgr.evaluateOrder(req);
    if (risk::RiskDecision::Rejected == eval.decision)
    {
        log_app->warn("Signal REJECTED [{}] {} {} @ {:.2f} — {}",
                      sig.strategy_id,
                      sig.symbol,
                      (Side::Buy == req.side) ? "BUY" : "SELL",
                      sig.price,
                      eval.reason_message);
        return;
    }

    // Apply risk-modified quantity.
    if (risk::RiskDecision::Modified == eval.decision)
    {
        req.quantity = eval.approved_qty;
        log_app->info("Signal MODIFIED: qty reduced to {:.6f}",
                      eval.approved_qty);
    }

    log_app->info("Placing {} order: {} {:.6f} {} @ ~{:.2f} "
                  "(conf={:.2f}, reason={})",
                  (Side::Buy == req.side) ? "BUY" : "SELL",
                  sig.strategy_id,
                  req.quantity,
                  req.symbol,
                  sig.price,
                  sig.confidence,
                  sig.reason);

    auto result = placeOrder(req);
    if (!ok(result))
    {
        log_app->error("Signal order FAILED: {} (code={})",
                       error(result).message,
                       static_cast<int>(error(result).code));
    }
}

Result<execution::OrderResponse>
OrderFlowExecutor::placeOrder(const execution::OrderRequest &req)
{
    auto log_app = logging::Logger::get("app");

    // 1. Risk evaluation (reserves notional budget atomically).
    auto eval = m_riskMgr.evaluateOrder(req);
    if (risk::RiskDecision::Rejected == eval.decision)
    {
        // Preserve the actual risk reason code (halt, limit hit, ...)
        // instead of flattening everything to OrderRejected.
        const auto code = (ErrorCode::Ok != eval.reason_code)
                              ? eval.reason_code : ErrorCode::OrderRejected;
        return PulseError{ code, eval.reason_message };
    }

    // 2. Apply risk-modified quantity.
    if (risk::RiskDecision::Modified == eval.decision)
    {
        log_app->info("Order MODIFIED: qty reduced to {:.6f}",
                      eval.approved_qty);
    }
    auto order_req = req;
    if (risk::RiskDecision::Modified == eval.decision)
    {
        order_req.quantity = eval.approved_qty;
    }

    // 3. Pick placer/tracker by market type.
    auto *placer  = (MarketType::Futures == order_req.market_type)
                        ? m_futuresPlacer : m_spotPlacer;
    auto *tracker = (MarketType::Futures == order_req.market_type)
                        ? m_futuresTracker : m_spotTracker;

    if (nullptr == placer)
    {
        if (eval.reservation_id > 0)
        {
            m_positionMgr.cancelReservation(eval.reservation_id);
        }
        return PulseError{ ErrorCode::InternalError,
                           "No executor for market_type="
                               + std::to_string(static_cast<int>(order_req.market_type))
                               + " — order aborted" };
    }

    // 4. Place order via REST (serialized with heartbeat/other callers).
    //    Futures leverage is applied first, in the same critical section —
    //    Gate.io sets leverage at the position level, not per order, so the
    //    account's current setting (e.g. 200x) would silently apply instead
    //    of the strategy's configured multiple.
    std::lock_guard rest_lock(m_restMutex);
    if (MarketType::Futures == order_req.market_type && order_req.leverage > 0.0)
    {
        auto lev = placer->setLeverage(order_req.symbol, order_req.leverage);
        if (!ok(lev))
        {
            const auto err = error(lev);
            if (eval.reservation_id > 0)
            {
                m_positionMgr.cancelReservation(eval.reservation_id);
            }
            return PulseError{ err.code,
                               "Leverage setup failed for " + order_req.symbol
                                   + ": " + err.message };
        }
    }

    auto result = placer->place(order_req);
    if (!ok(result))
    {
        auto err = error(result);
        if (eval.reservation_id > 0)
        {
            m_positionMgr.cancelReservation(eval.reservation_id);
        }
        return err;
    }

    auto &resp = value(result);
    log_app->info("Order PLACED: id={} status={}",
                  resp.order_id,
                  static_cast<int>(resp.status));

    // 5. Record reservation for the completion handler to consume.
    if (eval.reservation_id > 0)
    {
        std::lock_guard lock(m_mutex);
        m_reservations[resp.order_id] = eval.reservation_id;
    }

    // 6. Track order lifecycle via WS + REST polling fallback.
    if (tracker)
    {
        tracker->trackOrder(resp.order_id,
                            order_req.symbol,
                            order_req.side,
                            order_req.type,
                            order_req.quantity,
                            order_req.price,
                            order_req.client_order_id);

        // Market orders fill within the same millisecond — usually BEFORE the
        // WS private-channel subscription in trackOrder() is established, so
        // no fill event ever arrives. Compensate with an immediate REST poll
        // so the fill lands in PositionManager right away instead of staying
        // Pending until a later reconcile.
        if (OrderStatus::Filled == resp.status)
        {
            auto poll = tracker->pollOrderStatus(resp.order_id);
            if (!ok(poll))
            {
                log_app->warn("Order {} filled on exchange but status poll failed: {}",
                              resp.order_id, error(poll).message);
            }
        }
    }

    return result;
}

bool OrderFlowExecutor::cancelOrder(const std::string &order_id)
{
    if (order_id.empty())
    {
        return false;
    }

    // Order IDs are unique per market — probe futures first, then spot.
    if (m_futuresTracker && m_futuresPlacer)
    {
        if (m_futuresTracker->getStatus(order_id).has_value())
        {
            return m_futuresPlacer->cancel(order_id);
        }
    }
    if (m_spotTracker && m_spotPlacer)
    {
        if (m_spotTracker->getStatus(order_id).has_value())
        {
            return m_spotPlacer->cancel(order_id);
        }
    }
    return false;
}

void OrderFlowExecutor::onOrderComplete(const execution::ExecutionReport &report)
{
    auto log_app = logging::Logger::get("app");

    log_app->info("Order COMPLETED: id={} {} {} {:.6f} @ {:.2f} "
                  "fees={:.4f} slippage={:.2f}bps latency={}ms",
                  report.order_id,
                  report.symbol,
                  (Side::Buy == report.side) ? "BUY" : "SELL",
                  report.filled_qty,
                  report.avg_fill_price,
                  report.fees,
                  report.slippage_bps,
                  report.latency.count());

    // Consume the notional reservation (both buy and sell branches).
    {
        std::lock_guard lock(m_mutex);
        auto it = m_reservations.find(report.order_id);
        if (it != m_reservations.end())
        {
            m_positionMgr.consumeReservation(it->second);
            m_reservations.erase(it);
        }
    }

    // Update position manager and compute realized PnL.
    double pnl = 0.0;
    if (Side::Buy == report.side)
    {
        auto open_result = m_positionMgr.openPosition(
            report.symbol,
            report.side,
            report.filled_qty,
            report.avg_fill_price,
            report.client_order_id);
        if (!ok(open_result))
        {
            log_app->warn("Failed to open position: {}",
                          error(open_result).message);
        }
    }
    else
    {
        // For sells, try to close matching positions.
        auto positions = m_positionMgr.getPositionsBySymbol(report.symbol);
        for (const auto &pos : positions)
        {
            auto close_result = m_positionMgr.closePosition(
                pos.position_id, report.filled_qty, report.avg_fill_price);
            if (close_result.has_value())
            {
                pnl += close_result.value();
                log_app->info("Closed position {} (realized PnL: {:.4f})",
                              pos.position_id, close_result.value());
            }
            else
            {
                log_app->warn("Failed to close position {}", pos.position_id);
            }
        }
    }

    // Update drawdown guard with realized PnL.
    m_drawdownGuard.recordPnl(pnl);

#ifdef PULSE_ENABLE_SQLITE
    if (m_tradeRecorder)
    {
        auto rec_result = m_tradeRecorder->recordTrade(
            report, pnl, report.client_order_id);
        if (!ok(rec_result))
        {
            log_app->warn("Trade recorder INSERT failed: {}",
                          error(rec_result).message);
        }
    }
#endif
}

} // namespace pulse::control
