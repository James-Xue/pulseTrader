// paper_venue.cpp — In-process simulated exchange for paper trading (P2: CFD).
//
// See PaperVenue.hpp for the two load-bearing invariants. Every monetary
// operation delegates to core/fill_math.hpp so the venue and the offline
// backtest cannot drift.

#include "execution/PaperVenue.hpp"

#include "core/PulseError.hpp"
#include "core/fill_math.hpp"
#include "logging/Logger.hpp"

#include <algorithm>
#include <chrono>

namespace pulse::execution
{

namespace
{

constexpr const char *kLogModule = "paper";

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

PaperVenue::PaperVenue(const PaperConfig &cfg, std::vector<Symbol> symbols,
                       PaperAccount &account)
    : m_cfg{ cfg }
    , m_account{ account }
    , m_symbols{ std::move(symbols) }
{
}

// ---------------------------------------------------------------------------
// Placer
// ---------------------------------------------------------------------------

Result<OrderResponse> PaperVenue::place(const OrderRequest &req)
{
    // 1. Scope + configuration gates. A symbol without a configured contract
    //    size is rejected rather than defaulted: a 100x quanto error would
    //    corrupt every notional and PnL number downstream.
    if (MarketType::Cfd != req.market_type)
    {
        return PulseError{ ErrorCode::InvalidOrder,
                           std::string{"paper venue: only CFD orders are "
                                       "simulated in P2 (got market_type "}
                               + toString(req.market_type) + ")" };
    }
    if (0 == m_cfg.contract_volume.count(req.symbol))
    {
        return PulseError{ ErrorCode::InvalidOrder,
                           "paper venue: symbol " + req.symbol
                               + " has no [paper.contract_volume] entry" };
    }
    if (req.quantity <= 0.0)
    {
        return PulseError{ ErrorCode::InvalidOrder,
                           "paper venue: quantity must be > 0" };
    }

    // 2. Price resolution: a market order needs a FRESH mark; a limit order
    //    carries its own price.
    const std::int64_t now_ms = nowMs();
    double price = req.price;
    if (OrderType::Market == req.type)
    {
        const auto mark = freshMark(req.symbol, MarketType::Cfd, now_ms);
        if (!mark.has_value())
        {
            return PulseError{ ErrorCode::InvalidOrder,
                               "paper venue: no fresh mark for " + req.symbol
                                   + " (market order refused)" };
        }
        price = mark->price;
    }
    else if (price <= 0.0)
    {
        return PulseError{ ErrorCode::InvalidOrder,
                           "paper venue: limit order needs a positive price" };
    }

    // 3. Affordability. The ledger is the venue's authoritative cash; the
    //    engine's M22 gate runs the same arithmetic against freeMargin().
    const double quanto = quantoFor(req.symbol);
    const double leverage = std::max(req.leverage, 1.0);
    const double notional = req.quantity * quanto * price;
    const double margin = core::fill_math::requiredMargin(notional, leverage);
    const double fee = commissionFor(req.side, req.quantity);
    if (!m_account.canAfford(PaperLeg::Cfd, margin + fee))
    {
        return PulseError{ ErrorCode::InsufficientFreeMargin,
                           "paper venue: insufficient margin (need "
                               + std::to_string(margin + fee) + ", free "
                               + std::to_string(m_account.snapshot().cfd
                                                    .cash_usdt)
                               + ")" };
    }

    // 4. Book the order ONLY. Filling here would run the engine's completion
    //    path before its reservation exists (see PaperVenue.hpp invariant 1).
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_seq;
    PendingOrder order;
    order.order_id = "paper-cfd-" + std::to_string(m_seq);
    order.client_order_id = req.client_order_id;
    order.symbol = req.symbol;
    order.side = req.side;
    order.type = req.type;
    order.requested_qty = req.quantity;
    order.limit_price = req.price;
    order.submit_mid_price = price;
    order.leverage = leverage;
    order.quanto = quanto;
    order.sl_price = req.sl_price.value_or(0.0);
    order.tp_price = req.tp_price.value_or(0.0);
    order.status = OrderStatus::Open;
    order.submit_time = std::chrono::system_clock::now();
    order.last_update_time = order.submit_time;
    m_orders[order.order_id] = order;

    OrderResponse response;
    response.order_id = order.order_id;
    response.status = OrderStatus::Open;
    response.submit_time = order.submit_time;
    return response;
}

bool PaperVenue::cancel(const std::string &order_id)
{
    CompletionCallback callback;
    ExecutionReport report;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_orders.find(order_id);
        if (it == m_orders.end())
        {
            return false;
        }
        if (it->second.terminal)
        {
            return false;
        }
        cancelOrderLocked(it->second, std::chrono::system_clock::now());
        report = buildFillReport(it->second, it->second.avg_fill_price,
                                 it->second.last_update_time);
        report.final_status = OrderStatus::Cancelled;
        m_reports.push_back(report);
        callback = m_completion;
    }
    if (callback)
    {
        callback(report);
    }
    return true;
}

Result<nlohmann::json>
PaperVenue::setLeverage(const std::string & /*contract*/, double /*leverage*/)
{
    return nlohmann::json::object();
}

// ---------------------------------------------------------------------------
// Tracker
// ---------------------------------------------------------------------------

void PaperVenue::trackOrder(const std::string &order_id,
                            const Symbol &symbol, Side side, OrderType type,
                            Quantity requested_qty, Price submit_mid_price,
                            const std::string &client_order_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_orders.find(order_id);
    if (it == m_orders.end())
    {
        // Defensive: trackOrder must be able to describe an order the placer
        // booked under a different key only if a caller synthesizes one.
        PendingOrder order;
        order.order_id = order_id;
        order.status = OrderStatus::Open;
        order.submit_time = std::chrono::system_clock::now();
        m_orders[order_id] = order;
        return;
    }
    PendingOrder &order = it->second;
    order.symbol = symbol;
    order.side = side;
    order.type = type;
    order.requested_qty = requested_qty;
    order.submit_mid_price = submit_mid_price;
    if (!client_order_id.empty())
    {
        order.client_order_id = client_order_id;
    }
}

std::optional<OrderStatus>
PaperVenue::getStatus(const std::string &order_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_orders.find(order_id);
    if (it == m_orders.end())
    {
        return std::nullopt;
    }
    return it->second.status;
}

void PaperVenue::setCompletionCallback(CompletionCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completion = std::move(callback);
}

Result<OrderStatus> PaperVenue::pollOrderStatus(const std::string &order_id)
{
    CompletionCallback callback;
    ExecutionReport report;
    OrderStatus status = OrderStatus::Pending;
    bool deliver = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_orders.find(order_id);
        if (it == m_orders.end())
        {
            return PulseError{ ErrorCode::InvalidOrder,
                               "paper venue: unknown order " + order_id };
        }
        PendingOrder &order = it->second;
        if (order.terminal)
        {
            return order.status;
        }

        const std::int64_t now_ms = nowMs();
        if (OrderType::Market == order.type)
        {
            const auto result = fillMarketOrder(order, now_ms);
            if (!ok(result))
            {
                return error(result);
            }
            status = value(result);
            if (!order.terminal)
            {
                return status; // stale mark: stay pending, retry next poll
            }
            report = buildFillReport(order, order.avg_fill_price,
                                     order.last_update_time);
            report.final_status = order.status;
            m_reports.push_back(report);
            callback = m_completion;
            deliver = true;
        }
        else
        {
            // A resting order fills only when the mark crosses its price.
            const auto mark = freshMarkLocked(order.symbol, MarketType::Cfd, now_ms);
            bool crossed = false;
            if (mark.has_value())
            {
                crossed = (Side::Buy == order.side)
                    ? mark->price <= order.limit_price
                    : mark->price >= order.limit_price;
            }
            if (!crossed)
            {
                return order.status;
            }
            const auto result = fillMarketOrder(order, now_ms);
            if (!ok(result))
            {
                return error(result);
            }
            status = value(result);
            if (order.terminal)
            {
                report = buildFillReport(order, order.avg_fill_price,
                                         order.last_update_time);
                report.final_status = order.status;
                m_reports.push_back(report);
                callback = m_completion;
                deliver = true;
            }
        }
    }
    if (deliver && callback)
    {
        callback(report);
    }
    return status;
}

void PaperVenue::reconcileAll() {}

void PaperVenue::reconcileAll(std::mutex & /*rest_mutex*/) {}

std::vector<OrderSnapshot> PaperVenue::activeOrders() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<OrderSnapshot> out;
    for (const auto &entry : m_orders)
    {
        const PendingOrder &order = entry.second;
        if (order.terminal)
        {
            continue;
        }
        OrderSnapshot snapshot;
        snapshot.order_id = order.order_id;
        snapshot.client_order_id = order.client_order_id;
        snapshot.symbol = order.symbol;
        snapshot.side = order.side;
        snapshot.type = order.type;
        snapshot.requested_qty = order.requested_qty;
        snapshot.filled_qty = order.filled_qty;
        snapshot.status = order.status;
        snapshot.submit_time = order.submit_time;
        snapshot.last_update_time = order.last_update_time;
        out.push_back(std::move(snapshot));
    }
    return out;
}

std::vector<ExecutionReport>
PaperVenue::recentReports(std::size_t n) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::size_t begin = (m_reports.size() > n) ? (m_reports.size() - n) : 0;
    return std::vector<ExecutionReport>(m_reports.begin() + static_cast<std::ptrdiff_t>(begin),
                                        m_reports.end());
}

void PaperVenue::recordCompletedReport(const ExecutionReport &report)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_reports.push_back(report);
}

std::string PaperVenue::resolveExchangeId(const std::string &order_id) const
{
    return order_id;
}

// ---------------------------------------------------------------------------
// Venue surface
// ---------------------------------------------------------------------------

void PaperVenue::setMarkProvider(PaperMarkProvider provider)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_markProvider = std::move(provider);
}

std::optional<PaperMark> PaperVenue::markFor(const Symbol &symbol,
                                             MarketType market_type) const
{
    PaperMarkProvider provider;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        provider = m_markProvider;
    }
    if (!provider)
    {
        return std::nullopt;
    }
    return provider(symbol, market_type);
}

namespace
{

std::optional<PaperMark> validateMark(const std::optional<PaperMark> &mark,
                                      std::int64_t now_ms,
                                      std::int64_t max_age_ms)
{
    if (!mark.has_value() || mark->price <= 0.0 || mark->timestamp_ms <= 0)
    {
        return std::nullopt;
    }
    // A backwards wall-clock step yields a negative age; clamp to fresh.
    const std::int64_t age = now_ms - mark->timestamp_ms;
    if (age > max_age_ms)
    {
        return std::nullopt;
    }
    return mark;
}

} // namespace

std::optional<PaperMark> PaperVenue::freshMark(const Symbol &symbol,
                                               MarketType market_type,
                                               std::int64_t now_ms) const
{
    PaperMarkProvider provider;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        provider = m_markProvider;
    }
    if (!provider)
    {
        return std::nullopt;
    }
    return validateMark(provider(symbol, market_type), now_ms,
                        m_cfg.max_mark_age_ms);
}

std::optional<PaperMark> PaperVenue::freshMarkLocked(
    const Symbol &symbol, MarketType market_type, std::int64_t now_ms) const
{
    if (!m_markProvider)
    {
        return std::nullopt;
    }
    return validateMark(m_markProvider(symbol, market_type), now_ms,
                        m_cfg.max_mark_age_ms);
}

double PaperVenue::quantoFor(const Symbol &symbol) const
{
    const auto it = m_cfg.contract_volume.find(symbol);
    return (it != m_cfg.contract_volume.end() && it->second > 0.0)
        ? it->second
        : 1.0;
}

double PaperVenue::commissionFor(Side side, double lots) const
{
    // Gate TradFi charges the commission on the BUY leg only.
    return (Side::Buy == side)
        ? m_cfg.cfd_commission_usdt_per_lot * lots
        : 0.0;
}

void PaperVenue::tick(std::int64_t now_ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_positions.begin(); it != m_positions.end();)
    {
        PaperPosition &position = *it;
        const auto mark = freshMarkLocked(position.symbol, MarketType::Cfd, now_ms);
        if (!mark.has_value())
        {
            ++it;
            continue;
        }

        const double mark_price = mark->price;
        const double pnl = core::fill_math::positionPnl(
            position.side, position.entry_price, mark_price, position.lots,
            position.quanto);

        PaperExitReason reason = PaperExitReason::StopLoss;
        double fill_price = 0.0;
        bool exit = false;

        const bool long_side = (Side::Buy == position.side);
        if (position.sl_price > 0.0
            && ((long_side && mark_price <= position.sl_price)
                || ((!long_side) && mark_price >= position.sl_price)))
        {
            // Gap-honest: the stop fills at the observed mark, not at the
            // stop level (a weekend gap must not be forgiven).
            exit = true;
            fill_price = mark_price;
            reason = PaperExitReason::StopLoss;
        }
        else if (position.tp_price > 0.0
                 && ((long_side && mark_price >= position.tp_price)
                     || ((!long_side) && mark_price <= position.tp_price)))
        {
            // Take-profit is a resting limit: it fills at its own price.
            exit = true;
            fill_price = position.tp_price;
            reason = PaperExitReason::TakeProfit;
        }
        else if (m_cfg.stop_out_enabled && position.locked_margin > 0.0
                 && pnl <= -position.locked_margin)
        {
            exit = true;
            fill_price = mark_price;
            reason = PaperExitReason::StopOut;
        }

        if (!exit)
        {
            ++it;
            continue;
        }

        closePositionLocked(position, position.lots, fill_price,
                            std::chrono::system_clock::now(),
                            /*emit_exit=*/true, reason);
        it = m_positions.erase(it);
    }
}

double PaperVenue::freeMargin(PaperLeg leg) const
{
    const PaperAccountSnapshot snapshot = m_account.snapshot();
    switch (leg)
    {
    case PaperLeg::Futures:
        return snapshot.futures.cash_usdt;
    case PaperLeg::Cfd:
        return snapshot.cfd.cash_usdt;
    default:
        return snapshot.spot.cash_usdt;
    }
}

double PaperVenue::unrealizedPnl(PaperLeg leg) const
{
    // pull-based provider reads; no callbacks, so holding the venue lock is
    // safe (and keeps the position list stable).
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::int64_t now_ms = nowMs();
    double total = 0.0;
    for (const PaperPosition &position : m_positions)
    {
        const MarketType market = MarketType::Cfd;
        if (legFor(market) != leg)
        {
            continue;
        }
        auto mark = freshMarkLocked(position.symbol, market, now_ms);
        const double price = mark.has_value() ? mark->price : position.entry_price;
        total += core::fill_math::positionPnl(
            position.side, position.entry_price, price, position.lots,
            position.quanto);
    }
    return total;
}

double PaperVenue::unrealizedPnlTotal() const
{
    return unrealizedPnl(PaperLeg::Spot) + unrealizedPnl(PaperLeg::Futures)
        + unrealizedPnl(PaperLeg::Cfd);
}

PaperAccountSnapshot PaperVenue::accountSnapshot() const
{
    return m_account.snapshot(unrealizedPnl(PaperLeg::Spot),
                              unrealizedPnl(PaperLeg::Futures),
                              unrealizedPnl(PaperLeg::Cfd));
}

std::vector<PaperPositionRow>
PaperVenue::openPositions(MarketType market_type) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::int64_t now_ms = nowMs();
    std::vector<PaperPositionRow> out;
    for (const PaperPosition &position : m_positions)
    {
        if (MarketType::Cfd != market_type)
        {
            continue;
        }
        PaperPositionRow row;
        row.symbol = position.symbol;
        row.exchange_position_id = position.exchange_position_id;
        row.side = position.side;
        row.lots = position.lots;
        row.entry_price = position.entry_price;
        const auto mark = freshMarkLocked(position.symbol, market_type, now_ms);
        row.mark_price = mark.has_value() ? mark->price : position.entry_price;
        row.leverage = position.leverage;
        row.locked_margin = position.locked_margin;
        row.sl_price = position.sl_price;
        row.tp_price = position.tp_price;
        row.open_time = position.open_time;
        out.push_back(std::move(row));
    }
    return out;
}

std::optional<PaperStopState>
PaperVenue::cfdStopState(const std::string &exchange_position_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const PaperPosition &position : m_positions)
    {
        if (position.exchange_position_id == exchange_position_id)
        {
            PaperStopState state;
            state.sl_price = position.sl_price;
            state.tp_price = position.tp_price;
            return state;
        }
    }
    return std::nullopt;
}

Result<PaperCfdClose>
PaperVenue::closeCfdPosition(const std::string &exchange_position_id,
                             double lots, double requested_qty_total)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_positions.begin(); it != m_positions.end(); ++it)
    {
        if (it->exchange_position_id != exchange_position_id)
        {
            continue;
        }
        PaperPosition &position = *it;
        const double close_lots =
            std::min(lots > 0.0 ? lots : position.lots, position.lots);
        const auto mark =
            freshMarkLocked(position.symbol, MarketType::Cfd, nowMs());
        if (!mark.has_value())
        {
            return PulseError{ ErrorCode::InvalidOrder,
                               "paper venue: no fresh mark to close "
                                   + position.symbol };
        }

        const double margin_before = position.locked_margin;
        const double fee_before = m_account.snapshot().cfd.fees_paid_usdt;
        const double realized = closePositionLocked(
            position, close_lots, mark->price,
            std::chrono::system_clock::now(), /*emit_exit=*/false,
            PaperExitReason::StopLoss);

        PaperCfdClose result;
        result.exit_price = mark->price;
        result.realized_pnl = realized;
        result.margin_released = margin_before - position.locked_margin;
        result.fee =
            m_account.snapshot().cfd.fees_paid_usdt - fee_before;

        if (position.lots <= 1e-9)
        {
            m_positions.erase(it);
        }

        PULSE_LOG_INFO(kLogModule,
                       "paper close {} lots={:.2f} @ {:.2f} pnl={:.2f} "
                       "margin_released={:.2f} fee={:.2f}",
                       exchange_position_id, close_lots, result.exit_price,
                       result.realized_pnl, result.margin_released,
                       result.fee);
        return result;
    }

    return PulseError{ ErrorCode::InvalidOrder,
                       "paper venue: position not found: "
                           + exchange_position_id };
}

Result<nlohmann::json>
PaperVenue::modifyCfdStops(const std::string &exchange_position_id,
                           double sl_price, double tp_price)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (PaperPosition &position : m_positions)
    {
        if (position.exchange_position_id != exchange_position_id)
        {
            continue;
        }
        position.sl_price = sl_price;
        position.tp_price = tp_price;
        return nlohmann::json{ { "result", "ok" },
                               { "position_id", exchange_position_id },
                               { "price_sl", sl_price },
                               { "price_tp", tp_price } };
    }
    return PulseError{ ErrorCode::InvalidOrder,
                       "paper venue: position not found: "
                           + exchange_position_id };
}

std::vector<PaperExitEvent> PaperVenue::drainExitEvents()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PaperExitEvent> out;
    out.swap(m_exits);
    return out;
}

std::size_t PaperVenue::openPositionCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_positions.size();
}

// ---------------------------------------------------------------------------
// Fill helpers (all callers hold m_mutex)
// ---------------------------------------------------------------------------

Result<OrderStatus> PaperVenue::fillMarketOrder(PendingOrder &order,
                                                std::int64_t now_ms)
{
    const auto mark = freshMarkLocked(order.symbol, MarketType::Cfd, now_ms);
    if (!mark.has_value())
    {
        return OrderStatus::Pending; // stale: caller retries next poll
    }

    double fill_price = (OrderType::Market == order.type)
        ? mark->price
        : order.limit_price;
    if (fill_price <= 0.0)
    {
        return PulseError{ ErrorCode::InvalidOrder,
                           "paper venue: non-positive fill price" };
    }

    // Directional slippage, in basis points.
    if (m_cfg.slippage_bps > 0.0 && OrderType::Market == order.type)
    {
        const double factor = m_cfg.slippage_bps / 10'000.0;
        fill_price *= (Side::Buy == order.side) ? (1.0 + factor)
                                                : (1.0 - factor);
    }

    const double notional = order.requested_qty * order.quanto * fill_price;
    const double margin =
        core::fill_math::requiredMargin(notional, order.leverage);
    const double fee = commissionFor(order.side, order.requested_qty);

    if (!m_account.canAfford(PaperLeg::Cfd, margin + fee))
    {
        // Never entered the book: a Cancelled terminal report is the correct
        // signal here (the order was accepted by place(), then failed).
        cancelOrderLocked(order, std::chrono::system_clock::now());
        return OrderStatus::Cancelled;
    }

    if (!m_account.lockMargin(PaperLeg::Cfd, margin))
    {
        cancelOrderLocked(order, std::chrono::system_clock::now());
        return OrderStatus::Cancelled;
    }
    m_account.chargeFee(PaperLeg::Cfd, fee);

    order.filled_qty = order.requested_qty;
    order.avg_fill_price = fill_price;
    order.fees = fee;
    order.last_update_time = std::chrono::system_clock::now();
    openPositionFromOrder(order, fill_price);
    order.status = OrderStatus::Filled;
    order.terminal = true;
    return OrderStatus::Filled;
}

void PaperVenue::openPositionFromOrder(PendingOrder &order, double fill_price)
{
    ++m_seq;
    PaperPosition position;
    position.exchange_position_id = "pp-" + std::to_string(m_seq);
    position.symbol = order.symbol;
    position.side = order.side;
    position.lots = order.filled_qty;
    position.entry_price = fill_price;
    position.leverage = order.leverage;
    position.quanto = order.quanto;
    position.locked_margin =
        core::fill_math::requiredMargin(order.filled_qty * order.quanto
                                            * fill_price,
                                        order.leverage);
    position.sl_price = order.sl_price;
    position.tp_price = order.tp_price;
    position.open_time = std::chrono::system_clock::now();
    position.client_order_id = order.client_order_id;
    order.exchange_position_id = position.exchange_position_id;
    m_positions.push_back(std::move(position));
}

void PaperVenue::cancelOrderLocked(PendingOrder &order, Timestamp now)
{
    order.status = OrderStatus::Cancelled;
    order.terminal = true;
    order.last_update_time = now;
}

ExecutionReport PaperVenue::buildFillReport(PendingOrder &order,
                                            double fill_price,
                                            Timestamp now) const
{
    ExecutionReport report;
    report.order_id = order.order_id;
    report.client_order_id = order.client_order_id;
    report.symbol = order.symbol;
    report.side = order.side;
    report.type = order.type;
    report.requested_qty = order.requested_qty;
    report.filled_qty = order.filled_qty;
    report.avg_fill_price = fill_price;
    report.submit_mid_price = order.submit_mid_price;
    report.slippage_bps =
        (order.submit_mid_price > 0.0 && fill_price > 0.0)
        ? ExecutionReport::calculateSlippageBps(fill_price,
                                                order.submit_mid_price,
                                                order.side)
        : 0.0;
    report.fees = order.fees;
    report.submit_time = order.submit_time;
    report.fill_time = now;
    report.final_status = order.status;
    report.exchange_position_id = order.exchange_position_id;
    return report;
}

double PaperVenue::closePositionLocked(PaperPosition &position, double lots,
                                       double fill_price, Timestamp now,
                                       bool emit_exit, PaperExitReason reason)
{
    const double realized = core::fill_math::positionPnl(
        position.side, position.entry_price, fill_price, lots, position.quanto);

    const double margin_part = (position.lots > 0.0)
        ? position.locked_margin * (lots / position.lots)
        : 0.0;
    // Closing a short is a BUY leg and pays the CFD commission.
    const double fee = commissionFor(Side::Sell == position.side ? Side::Buy
                                                                 : Side::Sell,
                                     lots);

    m_account.releaseMargin(PaperLeg::Cfd, margin_part, realized);
    m_account.chargeFee(PaperLeg::Cfd, fee);

    position.lots -= lots;
    position.locked_margin -= margin_part;
    if (position.lots < 1e-9)
    {
        position.lots = 0.0;
        position.locked_margin = 0.0;
    }

    if (emit_exit)
    {
        PaperExitEvent event;
        event.exchange_position_id = position.exchange_position_id;
        event.symbol = position.symbol;
        event.position_side = position.side;
        event.closed_qty = lots;
        event.fill_price = fill_price;
        event.fee = fee;
        event.fill_time = now;
        event.reason = reason;
        m_exits.push_back(std::move(event));
    }

    return realized;
}

} // namespace pulse::execution
