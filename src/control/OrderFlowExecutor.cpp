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
void OrderFlowExecutor::setSymbolRegistry(
    std::shared_ptr<const market::SymbolRegistry> registry)
{
    m_registry = std::move(registry);
}

double OrderFlowExecutor::quantoMultiplierFor(const Symbol &symbol) const
{
    if (m_registry)
    {
        if (const auto info = m_registry->get(symbol))
        {
            return info->quanto_multiplier;
        }
    }
    return 1.0;   // Unknown/spot: 1 unit = 1 base-currency unit.
}

OrderFlowExecutor::OrderFlowExecutor(
    const StrategyConfig &strategy_cfg,
    risk::RiskManager &risk_mgr,
    risk::PositionManager &position_mgr,
    risk::DrawdownGuard &drawdown_guard,
    IOrderPlacer *spot_placer,
    IOrderPlacer *futures_placer,
    IOrderPlacer *cfd_placer,
    execution::OrderTracker *spot_tracker,
    execution::OrderTracker *futures_tracker,
    execution::OrderTracker *cfd_tracker,
    market::OrderBookManager *spot_order_book,
    market::OrderBookManager *futures_order_book,
    std::mutex &rest_mutex,
#ifdef PULSE_ENABLE_SQLITE
    trade_recorder::TradeRecorder *trade_recorder
#else
    std::nullptr_t /*trade_recorder*/
#endif
)
    : m_strategyCfg{ strategy_cfg }
    , m_signalOnly{ strategy_cfg.signal_only }
    , m_riskMgr{ risk_mgr }
    , m_positionMgr{ position_mgr }
    , m_drawdownGuard{ drawdown_guard }
    , m_spotPlacer{ spot_placer }
    , m_futuresPlacer{ futures_placer }
    , m_cfdPlacer{ cfd_placer }
    , m_spotTracker{ spot_tracker }
    , m_futuresTracker{ futures_tracker }
    , m_cfdTracker{ cfd_tracker }
    , m_spotOrderBook{ spot_order_book }
    , m_futuresOrderBook{ futures_order_book }
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
    if (m_cfdTracker)
    {
        m_cfdTracker->setCompletionCallback(
            [this](const execution::ExecutionReport &report)
            { onOrderComplete(report); });
    }
}

void OrderFlowExecutor::setActiveMarket(MarketType mt)
{
    m_activeMarket.store(mt, std::memory_order_relaxed);
}

MarketType OrderFlowExecutor::activeMarket() const
{
    return m_activeMarket.load(std::memory_order_acquire);
}

bool OrderFlowExecutor::signalOnly() const
{
    return m_signalOnly;
}

std::optional<execution::OrderRequest>
OrderFlowExecutor::buildRequestFromSignal(const strategy::TradingSignal &sig) const
{
    execution::OrderRequest req;
    req.symbol      = sig.symbol;
    req.side        = (strategy::SignalType::Buy == sig.type) ? Side::Buy : Side::Sell;
    req.type        = OrderType::Market;
    req.price       = sig.price;
    req.market_type = sig.market_type;
    // Futures qty is in contracts: the risk gate must compute notional as
    // qty * price * quanto_multiplier, or a 1-contract order (≈6 USDT for
    // BTC_USDT) is treated as 1 BTC (≈63k USDT) and capped to a sub-contract
    // size that the exchange then rejects.
    req.quanto_multiplier = quantoMultiplierFor(sig.symbol);

    // Find strategy config for leverage/quantity settings.
    // Signal aggregator uses strategy_id "signal_aggregator" which won't
    // match any instance name — fall back to first strategy on the same symbol.
    const auto *inst = matchInstanceConfig(sig);
    req.quantity = 0.001;
    if (inst)
    {
        req.quantity    = inst->order_quantity;
        req.market_type = inst->market_type;
        req.leverage    = inst->leverage;
    }

    // For futures: convert quantity to contract_size (integer contracts).
    if (MarketType::Futures == req.market_type && 0 == req.contract_size)
    {
        req.contract_size = static_cast<int>(std::max(1.0, std::round(req.quantity)));
    }

    // Maker pricing: post_only / maker_first configs place a POST-ONLY order
    // at the exact best bid (buy) / best ask (sell) from the live book.
    //
    // No book data → behavior depends on the config: maker_first (which is
    // allowed to take liquidity) falls back to a market order; plain post_only
    // (which by definition never crosses the spread) drops the signal — a
    // stale or guessed price would be rejected by the exchange anyway.
    if (inst && OrderType::Market != inst->order_type)
    {
        if (MarketType::Cfd == req.market_type)
        {
            // Unreachable (config validator rejects non-market on CFD),
            // defensive only: keep the market order.
            logging::Logger::get("app")->debug(
                "Signal [{}]: order_type {} on cfd is unsupported — market order",
                sig.strategy_id, toString(inst->order_type));
        }
        else
        {
            auto *book = (MarketType::Futures == req.market_type)
                             ? m_futuresOrderBook
                             : m_spotOrderBook;
            const auto price = bestBookPrice(book, sig.symbol, req.side);
            if (price.has_value())
            {
                req.type  = OrderType::PostOnly;
                req.price = *price;
            }
            else if (OrderType::MakerFirst == inst->order_type)
            {
                logging::Logger::get("app")->debug(
                    "Signal [{}]: no order book for {} — falling back to "
                    "market order",
                    sig.strategy_id, sig.symbol);
            }
            else
            {
                logging::Logger::get("app")->warn(
                    "Signal [{}]: no order book for {} — post_only signal "
                    "dropped",
                    sig.strategy_id, sig.symbol);
                return std::nullopt; // Signal dropped — never take liquidity.
            }
        }
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

    // Signal-only mode: signals are published to the board by the wiring
    // in main.cpp, but nothing may be placed from the aggregator path.
    if (m_signalOnly)
    {
        return;
    }

    // Direction gate: only the active market's signals may trade.
    // Skipped BEFORE risk evaluation so gated signals burn no rate-limiter
    // tokens and create no reservations.
    const MarketType active = m_activeMarket.load(std::memory_order_acquire);
    if (sig.market_type != active)
    {
        log_app->info("Signal SKIPPED [{}] {} {} — market {} not active "
                      "(active: {})",
                      sig.strategy_id,
                      sig.symbol,
                      (strategy::SignalType::Buy == sig.type) ? "BUY" : "SELL",
                      toString(sig.market_type),
                      toString(active));
        return;
    }

    auto req_opt = buildRequestFromSignal(sig);
    if (!req_opt.has_value())
    {
        // post_only signal with no book data — already logged, never trades.
        return;
    }
    auto req = *req_opt;

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

    // Pass the evaluation down: placeOrder must NOT re-evaluate, or the
    // caller's own reservation (created above) is double-counted and every
    // Modified order is rejected against its own budget.
    auto result = placeOrder(req, eval);
    if (!ok(result))
    {
        log_app->error("Signal order FAILED: {} (code={})",
                       error(result).message,
                       static_cast<int>(error(result).code));
        return;
    }

    // Maker-first: register a fallback attempt (deadline = now + timeout).
    // Only for signals whose config is maker_first and whose request was
    // actually placed as post-only (i.e. the book was available). The sweep
    // cancels expired attempts and re-issues the remainder as a market order.
    if (OrderType::PostOnly == req.type)
    {
        const auto *inst = matchInstanceConfig(sig);
        if (inst && OrderType::MakerFirst == inst->order_type
            && inst->maker_timeout_ms > 0)
        {
            std::lock_guard lock(m_mutex);
            m_makerAttempts[value(result).order_id] = MakerAttempt{
                std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(inst->maker_timeout_ms),
                req,
                req.market_type };
            log_app->info("Maker-first attempt registered: id={} fallback in {}ms",
                          value(result).order_id, inst->maker_timeout_ms);
        }
    }
}

Result<execution::OrderResponse>
OrderFlowExecutor::placeOrder(const execution::OrderRequest &req)
{
    // Direction gate (strategy-originated orders, maker-first fallback, close
    // flows): reject non-active markets unless the order is reduce_only
    // (closing old-direction positions stays possible). Manual orders take
    // placeManualOrder(), which also allows futures/CFD in any direction.
    const MarketType active = m_activeMarket.load(std::memory_order_acquire);
    if (req.market_type != active && !req.reduce_only)
    {
        return PulseError{ ErrorCode::InactiveMarket,
                           "order market_type=" + std::string(toString(req.market_type))
                               + " is not the active direction (active="
                               + toString(active) + ")" };
    }
    return evaluateAndPlace(req);
}

Result<execution::OrderResponse>
OrderFlowExecutor::placeManualOrder(const execution::OrderRequest &req)
{
    // Direction gate (manual orders, M22): futures and CFD orders are
    // executable in any active direction — per-market notional budgets
    // (maxPositionNotional{Futures,Cfd}) bound each market independently, so
    // the sub-agent can execute signals from both markets without switching
    // directions. Spot still requires the active direction (or reduce_only).
    const MarketType active = m_activeMarket.load(std::memory_order_acquire);
    if (req.market_type != active && !req.reduce_only
        && MarketType::Spot == req.market_type)
    {
        return PulseError{ ErrorCode::InactiveMarket,
                           std::string("order market_type=spot is not the active direction (active=")
                               + toString(active) + ")" };
    }
    return evaluateAndPlace(req);
}

Result<execution::OrderResponse>
OrderFlowExecutor::evaluateAndPlace(const execution::OrderRequest &req)
{
    // Evaluate exactly once, then place using that evaluation. Re-evaluating
    // after a Modified result double-counts the caller's own reservation and
    // rejects orders whose quantity was capped to the notional budget.
    auto eval = m_riskMgr.evaluateOrder(req);
    if (risk::RiskDecision::Rejected == eval.decision)
    {
        // Preserve the actual risk reason code (halt, limit hit, ...)
        // instead of flattening everything to OrderRejected.
        const auto code = (ErrorCode::Ok != eval.reason_code)
                              ? eval.reason_code : ErrorCode::OrderRejected;
        return PulseError{ code, eval.reason_message };
    }
    return placeOrder(req, eval);
}

Result<execution::OrderResponse>
OrderFlowExecutor::placeOrder(const execution::OrderRequest &req,
                              const risk::RiskEvalResult &eval)
{
    auto log_app = logging::Logger::get("app");

    // 2. Apply risk-modified quantity.
    auto order_req = req;
    if (risk::RiskDecision::Modified == eval.decision)
    {
        order_req.quantity = eval.approved_qty;
        log_app->info("Order MODIFIED: qty reduced to {:.6f}",
                      eval.approved_qty);
    }

    // 3. Pick placer/tracker by market type.
    IOrderPlacer *placer = m_spotPlacer;
    execution::OrderTracker *tracker = m_spotTracker;
    switch (order_req.market_type)
    {
    case MarketType::Futures:
        placer  = m_futuresPlacer;
        tracker = m_futuresTracker;
        break;
    case MarketType::Cfd:
        placer  = m_cfdPlacer;
        tracker = m_cfdTracker;
        break;
    default:
        break; // Spot.
    }

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
    //    The placed request is stored too: on fill the position must open
    //    with the same market type, leverage, and contract multiplier.
    if (eval.reservation_id > 0)
    {
        std::lock_guard lock(m_mutex);
        m_reservations[resp.order_id] = ReservationEntry{ eval.reservation_id, order_req };
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
        //
        // CFD market orders: parseOrderResponse cannot know the fill (the
        // POST response carries only an internal submission id), so poll
        // immediately too — pollCfdOrderStatus detects the fill via the
        // positions fallback and drives the completion callback.
        const bool needs_immediate_poll =
            (OrderStatus::Filled == resp.status)
            || (MarketType::Cfd == order_req.market_type
                && OrderType::Market == order_req.type);
        if (needs_immediate_poll)
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

    // Order IDs are unique per market — probe cfd, then futures, then spot.
    if (m_cfdTracker && m_cfdPlacer)
    {
        if (m_cfdTracker->getStatus(order_id).has_value())
        {
            // TradFi CFD: the tracked key may be the POST's data.id while the
            // exchange carries the real order id (list-read lag — the cancel
            // would 400 on data.id). Poll once to trigger the re-anchor, then
            // cancel the resolved real id.
            (void)m_cfdTracker->pollOrderStatus(order_id);
            const std::string real_id =
                m_cfdTracker->resolveExchangeId(order_id);
            return m_cfdPlacer->cancel(real_id);
        }
    }
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

int OrderFlowExecutor::cancelAllOpenOrders(MarketType mt)
{
    execution::OrderTracker *tracker = m_spotTracker;
    IOrderPlacer *placer = m_spotPlacer;
    switch (mt)
    {
    case MarketType::Futures:
        tracker = m_futuresTracker;
        placer  = m_futuresPlacer;
        break;
    case MarketType::Cfd:
        tracker = m_cfdTracker;
        placer  = m_cfdPlacer;
        break;
    default:
        break; // Spot.
    }

    if (nullptr == tracker || nullptr == placer)
    {
        return 0;
    }

    auto log_app = logging::Logger::get("app");
    int cancelled = 0;

    // Bounded sweep: reconcile → cancel → recheck. The direction gate already
    // closed before this runs, but a signal may have been mid-flight when the
    // switch happened — a couple of passes closes the race.
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        tracker->reconcileAll();
        const auto active = tracker->activeOrders();
        if (active.empty())
        {
            break;
        }
        for (const auto &order : active)
        {
            if (placer->cancel(order.order_id))
            {
                ++cancelled;
            }
        }
    }

    log_app->info("Direction switch: cancelled {} {} open orders",
                  cancelled, toString(mt));
    return cancelled;
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
    // The placed request is kept so the position opens with the same market
    // type, leverage, and contract multiplier the order was risk-checked with.
    ReservationEntry reservation;
    {
        std::lock_guard lock(m_mutex);
        m_makerAttempts.erase(report.order_id);   // Terminal report ends any attempt.
        auto it = m_reservations.find(report.order_id);
        if (it != m_reservations.end())
        {
            reservation = it->second;
            // The reservation may already be released by the maker-first
            // fallback (sweep zeroes reservation_id but keeps the request so
            // a partial fill still opens with correct market metadata).
            if (it->second.reservation_id > 0)
            {
                m_positionMgr.consumeReservation(it->second.reservation_id);
            }
            m_reservations.erase(it);
        }
    }

    // Update position manager and compute realized PnL.
    //
    // A fill in direction D first closes opposite-direction positions
    // (realized PnL), then opens the remaining quantity in direction D.
    // The open-remainder branch matters for shorts: a SELL fill with no long
    // to close must record the short, or the risk gate stays blind to real
    // short exposure.
    double pnl = 0.0;
    double remaining = report.filled_qty;
    auto positions = m_positionMgr.getPositionsBySymbol(report.symbol);
    for (const auto &pos : positions)
    {
        if (pos.side == report.side)
        {
            continue;   // Same direction — not a close.
        }
        const double closed_qty = std::min(remaining, pos.quantity);
        auto close_result = m_positionMgr.closePosition(
            pos.position_id, closed_qty, report.avg_fill_price);
        if (close_result.has_value())
        {
            pnl += close_result.value();
            remaining -= closed_qty;
            log_app->info("Closed position {} (realized PnL: {:.4f})",
                          pos.position_id, close_result.value());
        }
        else
        {
            log_app->warn("Failed to close position {}", pos.position_id);
        }
    }

    // Open the unfilled remainder in the fill direction (long or short).
    if (remaining > 0.0)
    {
        // Leverage-backed markets (futures + CFD) record the requested
        // leverage on the position for margin/PnL math; spot uses 1.0.
        const MarketType mt = reservation.request.market_type;
        const bool leverage_market =
            (MarketType::Futures == mt || MarketType::Cfd == mt);
        auto open_result = m_positionMgr.openPosition(
            report.symbol,
            report.side,
            remaining,
            report.avg_fill_price,
            report.client_order_id,
            mt,
            leverage_market ? reservation.request.leverage : 1.0,
            MarginMode::Cross,
            reservation.request.quanto_multiplier,
            0.005,
            // Carry the exchange-native protective stops (CFD) attached to
            // the entry order onto the tracked position, so get_positions
            // reflects them immediately after the fill.
            reservation.request.sl_price.value_or(0.0),
            reservation.request.tp_price.value_or(0.0));
        if (!ok(open_result))
        {
            log_app->warn("Failed to open position: {}",
                          error(open_result).message);
        }
        else if (!report.exchange_position_id.empty())
        {
            // TradFi CFD: record the exchange position id on the engine
            // position so close_position can call the real close endpoint
            // (the internal position_id is engine-local).
            m_positionMgr.setExchangePositionId(
                value(open_result), report.exchange_position_id);
        }
    }

    // Update drawdown guard with realized PnL.
    m_drawdownGuard.recordPnl(pnl);

    // Market metadata from the reservation's request (defaults = spot).
    const MarketType mt = reservation.request.market_type;
    const double lev = (MarketType::Spot == mt
                        || reservation.request.leverage <= 0.0)
                           ? 1.0
                           : reservation.request.leverage;

    recordCompletedTrade(report, pnl, mt, lev,
                         reservation.request.quanto_multiplier);
}

void OrderFlowExecutor::recordCfdClose(const execution::ExecutionReport &report,
                                       double pnl, double leverage)
{
    auto log_app = logging::Logger::get("app");

    log_app->info("CFD close recorded: id={} {} {} {:.6f} @ {:.2f} pnl={:.4f}",
                  report.order_id,
                  report.symbol,
                  (Side::Buy == report.side) ? "BUY" : "SELL",
                  report.filled_qty,
                  report.avg_fill_price,
                  pnl);

    if (m_cfdTracker)
    {
        m_cfdTracker->recordCompletedReport(report);
    }
    m_drawdownGuard.recordPnl(pnl);

    recordCompletedTrade(report, pnl, MarketType::Cfd,
                         leverage > 0.0 ? leverage : 1.0,
                         quantoMultiplierFor(report.symbol));
}

void OrderFlowExecutor::recordExternalClose(const execution::ExecutionReport &report,
                                            double pnl, MarketType market_type,
                                            double leverage)
{
    auto log_app = logging::Logger::get("app");

    // exit price is a best-effort estimate from the last known mark — flag
    // it so consumers never mistake it for an exchange-confirmed fill.
    log_app->info("EXTERNAL close recorded (exit price ~estimate): id={} "
                  "{} {} {:.6f} @ ~{:.2f} pnl={:.4f}",
                  report.order_id,
                  report.symbol,
                  (Side::Buy == report.side) ? "BUY" : "SELL",
                  report.filled_qty,
                  report.avg_fill_price,
                  pnl);

    execution::OrderTracker *tracker = nullptr;
    if (MarketType::Cfd == market_type)
    {
        tracker = m_cfdTracker;
    }
    else if (MarketType::Futures == market_type)
    {
        tracker = m_futuresTracker;
    }
    else
    {
        tracker = m_spotTracker;
    }
    if (tracker)
    {
        tracker->recordCompletedReport(report);
    }
    m_drawdownGuard.recordPnl(pnl);

    recordCompletedTrade(report, pnl, market_type,
                         leverage > 0.0 ? leverage : 1.0,
                         quantoMultiplierFor(report.symbol));
}

void OrderFlowExecutor::recordCompletedTrade(
    const execution::ExecutionReport &report, double pnl, MarketType market_type,
    double leverage, double quanto_multiplier)
{
#ifdef PULSE_ENABLE_SQLITE
    if (!m_tradeRecorder)
    {
        return;
    }
    auto log_app = logging::Logger::get("app");

    // Strategy name: prefer the matched instance (signals usually carry
    // an empty client_order_id); fall back to the client id.
    strategy::TradingSignal sig;
    sig.symbol = report.symbol;
    const auto *inst = matchInstanceConfig(sig);
    const std::string strategy_name =
        inst ? inst->name : report.client_order_id;

    auto rec_result = m_tradeRecorder->recordTrade(
        report, pnl, strategy_name, market_type, leverage, quanto_multiplier);
    if (!ok(rec_result))
    {
        log_app->warn("Trade recorder INSERT failed: {}",
                      error(rec_result).message);
    }
#else
    (void)report;
    (void)pnl;
    (void)market_type;
    (void)leverage;
    (void)quanto_multiplier;
#endif
}

// ---------------------------------------------------------------------------
// Maker-first helpers
// ---------------------------------------------------------------------------

const StrategyInstanceConfig *
OrderFlowExecutor::matchInstanceConfig(const strategy::TradingSignal &sig) const noexcept
{
    // Signal aggregator uses strategy_id "signal_aggregator" which won't
    // match any instance name — fall back to first strategy on the same symbol.
    for (const auto &inst : m_strategyCfg.strategies)
    {
        if (inst.name == sig.strategy_id)
        {
            return &inst;
        }
    }
    for (const auto &inst : m_strategyCfg.strategies)
    {
        if (inst.enabled && inst.symbol == sig.symbol)
        {
            return &inst;
        }
    }
    return nullptr;
}

std::optional<Price>
OrderFlowExecutor::bestBookPrice(market::OrderBookManager *book,
                                 const Symbol &symbol,
                                 Side side) const noexcept
{
    if (nullptr == book)
    {
        return std::nullopt;
    }
    if (Side::Buy == side)
    {
        const auto levels = book->topBids(symbol, 1);
        if (levels.empty())
        {
            return std::nullopt;
        }
        return levels.front().price;
    }
    const auto levels = book->topAsks(symbol, 1);
    if (levels.empty())
    {
        return std::nullopt;
    }
    return levels.front().price;
}

execution::OrderTracker *
OrderFlowExecutor::trackerFor(MarketType mt) const noexcept
{
    switch (mt)
    {
    case MarketType::Futures:
        return m_futuresTracker;
    case MarketType::Cfd:
        return m_cfdTracker;
    default:
        return m_spotTracker;
    }
}

void OrderFlowExecutor::eraseAttempt(const std::string &order_id)
{
    std::lock_guard lock(m_mutex);
    m_makerAttempts.erase(order_id);
}

// ---------------------------------------------------------------------------
// Maker-first sweep — cancel expired post-only attempts, fall back to market
//
// Lock order is strictly rest_mutex → m_mutex (matching placeOrder): the
// sweep holds rest_mutex ONLY around cancelOrder, takes m_mutex only after
// releasing it, and calls placeOrder (which locks rest_mutex internally)
// with no locks held.
// ---------------------------------------------------------------------------
void OrderFlowExecutor::sweepMakerAttempts()
{
    auto log_app = logging::Logger::get("app");

    // Phase 1 — snapshot expired attempts (no erasure yet: a failed cancel
    // must keep the attempt alive for the next sweep).
    struct Expired
    {
        std::string order_id;
        MakerAttempt attempt;
    };
    std::vector<Expired> expired;
    {
        std::lock_guard lock(m_mutex);
        if (m_makerAttempts.empty())
        {
            return;   // Fast path.
        }
        const auto now = std::chrono::steady_clock::now();
        for (const auto &[id, att] : m_makerAttempts)
        {
            if (now >= att.deadline)
            {
                expired.push_back({ id, att });
            }
        }
    }
    if (expired.empty())
    {
        return;
    }

    for (auto &e : expired)
    {
        // Phase 2 — is the order still live, and how much has filled?
        // activeOrders() is a shared-lock snapshot; filled_qty is kept live
        // by WS updates + REST polls. No REST traffic here.
        auto *tracker = trackerFor(e.attempt.market_type);
        if (nullptr == tracker)
        {
            eraseAttempt(e.order_id);
            continue;
        }
        const auto live = tracker->activeOrders();
        double filled = 0.0;
        bool found = false;
        for (const auto &snap : live)
        {
            if (snap.order_id == e.order_id)
            {
                found = true;
                filled = snap.filled_qty;
                break;
            }
        }
        if (!found)
        {
            // Terminal already (fill or exchange-side cancel) — the report
            // path (onOrderComplete) owns cleanup; just drop the attempt.
            eraseAttempt(e.order_id);
            continue;
        }
        const double remaining = e.attempt.request.quantity - filled;
        if (remaining <= 1e-9)
        {
            // Fully filled — the Filled report will handle it.
            eraseAttempt(e.order_id);
            continue;
        }

        // Phase 3 — cancel under rest_mutex (cancelOrder's contract). A
        // cancel the exchange refuses (already filled / rejected) means do
        // NOT fall back — chasing a filled order would double-open. The
        // attempt stays alive; the next sweep either sees it gone (terminal
        // report arrived) or retries the cancel.
        bool cancel_ok;
        {
            std::lock_guard rest_lock(m_restMutex);
            cancel_ok = cancelOrder(e.order_id);
        }
        if (!cancel_ok)
        {
            continue;
        }

        // Phase 4 — release the old reservation but KEEP the entry with
        // reservation_id = 0: the Cancelled report for this order is on its
        // way, and it still needs the request metadata to open any partial
        // fill with the correct market type / leverage / multiplier.
        // consumeReservation(0) is a no-op (ids start at 1).
        {
            std::lock_guard lock(m_mutex);
            auto it = m_reservations.find(e.order_id);
            if (it != m_reservations.end())
            {
                if (it->second.reservation_id > 0)
                {
                    m_positionMgr.cancelReservation(it->second.reservation_id);
                }
                it->second.reservation_id = 0;
            }
            m_makerAttempts.erase(e.order_id);
        }

        // Phase 5 — fresh market fallback for the REMAINDER. Full re-eval:
        // new rate-limiter token, new notional reservation. No locks held
        // here — placeOrder(req) locks rest_mutex (then m_mutex) internally.
        auto fallback = e.attempt.request;
        fallback.type     = OrderType::Market;
        fallback.price    = 0.0;
        fallback.quantity = remaining;
        auto res = placeOrder(fallback);
        if (!ok(res))
        {
            log_app->warn(
                "Maker-first fallback FAILED for {}: {} (code={}) — partial "
                "maker fill (if any) stands, remainder not entered",
                e.order_id, error(res).message,
                static_cast<int>(error(res).code));
        }
        else
        {
            log_app->info("Maker-first fallback: cancelled {}, re-issued "
                          "market {:.6f} {} -> {}",
                          e.order_id, remaining, fallback.symbol,
                          value(res).order_id);
        }
    }
}

} // namespace pulse::control
