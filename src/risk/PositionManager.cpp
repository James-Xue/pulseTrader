// positionManager.cpp — Thread-safe position tracking (Layer 7 Risk Management)

#include "risk/PositionManager.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <cmath>

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PositionManager::PositionManager(const RiskConfig &config)
    : m_config{ config }
{
}

// ---------------------------------------------------------------------------
// Position lifecycle
// ---------------------------------------------------------------------------

Result<std::string> PositionManager::openPosition(
    const Symbol &symbol, Side side, Quantity qty, Price entry_price,
    const std::string &strategy_id)
{
    // Delegate to the full overload with spot defaults.
    return openPosition(symbol, side, qty, entry_price, strategy_id,
                         MarketType::Spot, 1.0, MarginMode::Cross, 1.0, 0.0);
}

Result<std::string> PositionManager::openPosition(
    const Symbol &symbol, Side side, Quantity qty, Price entry_price,
    const std::string &strategy_id,
    MarketType market_type, double leverage, MarginMode margin_mode,
    double quanto_multiplier, double maintenance_rate)
{
    // Open a new position after validating all portfolio limits:
    // 1. Acquire exclusive (write) lock
    // 2. Compute proposed notional: qty * entry_price * quanto_multiplier
    //    (for spot, quanto_multiplier=1.0 so this is just qty * entry_price)
    // 3. Check total notional + proposed <= maxPositionNotional
    // 4. Check open position count < maxOpenPositions
    // 5. Check per-symbol notional + proposed <= maxSymbolNotional
    // 6. Generate unique position_id
    // 7. Create Position, insert into map, return position_id

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    const double proposed_notional = qty * entry_price * quanto_multiplier;

    // 3. Check total notional limit.
    double total_notional = 0.0;
    for (const auto &[id, pos] : m_positions)
    {
        total_notional += pos.notional_value;
    }

    if (total_notional + proposed_notional > m_config.maxPositionNotional)
    {
        PULSE_LOG_WARN("risk",
            "Position rejected: total notional {:.2f} + proposed {:.2f} exceeds limit {:.2f}",
            total_notional, proposed_notional, m_config.maxPositionNotional);
        return PulseError{ ErrorCode::PositionLimitHit,
            "Total notional limit exceeded" };
    }

    // 4. Check open position count limit.
    if (static_cast<int>(m_positions.size()) >= m_config.maxOpenPositions)
    {
        PULSE_LOG_WARN("risk",
            "Position rejected: {} open positions >= limit {}",
            m_positions.size(), m_config.maxOpenPositions);
        return PulseError{ ErrorCode::PositionLimitHit,
            "Maximum open positions reached" };
    }

    // 5. Check per-symbol notional limit.
    double sym_notional = 0.0;
    for (const auto &[id, pos] : m_positions)
    {
        if (pos.symbol == symbol)
        {
            sym_notional += pos.notional_value;
        }
    }

    if (sym_notional + proposed_notional > m_config.maxSymbolNotional)
    {
        PULSE_LOG_WARN("risk",
            "Position rejected: symbol {} notional {:.2f} + proposed {:.2f} exceeds limit {:.2f}",
            symbol, sym_notional, proposed_notional, m_config.maxSymbolNotional);
        return PulseError{ ErrorCode::SymbolLimitHit,
            "Per-symbol notional limit exceeded" };
    }

    // 6. Generate unique position ID.
    std::string pos_id = generatePositionId(symbol, side);

    // 7. Create Position and insert.
    Position pos;
    pos.position_id = pos_id;
    pos.symbol = symbol;
    pos.side = side;
    pos.quantity = qty;
    pos.entry_price = entry_price;
    pos.current_price = entry_price; // Initially mark at entry.
    pos.unrealized_pnl = 0.0;
    pos.notional_value = proposed_notional;
    pos.open_time = now();
    pos.strategy_id = strategy_id;

    // Futures-specific fields.
    pos.market_type = market_type;
    pos.leverage = leverage;
    pos.margin_mode = margin_mode;
    pos.quanto_multiplier = quanto_multiplier;

    // Margin calculation (futures only; spot = 0).
    if (MarketType::Futures == market_type && leverage > 0.0)
    {
        pos.margin_used = qty * entry_price * quanto_multiplier / leverage;
    }
    else
    {
        pos.margin_used = 0.0;
    }

    // Estimated liquidation price (futures only).
    if (MarketType::Futures == market_type && leverage > 1.0)
    {
        if (Side::Buy == side)
        {
            // Long: liq = entry * (1 - 1/leverage + maintenance_rate)
            pos.liquidation_price = entry_price * (1.0 - 1.0 / leverage + maintenance_rate);
        }
        else
        {
            // Short: liq = entry * (1 + 1/leverage - maintenance_rate)
            pos.liquidation_price = entry_price * (1.0 + 1.0 / leverage - maintenance_rate);
        }
    }
    else
    {
        pos.liquidation_price = 0.0;
    }

    m_positions[pos_id] = pos;

    // Auto-consume a matching pending reservation (same symbol).
    // In the normal flow, reserveNotional() creates a reservation, then
    // openPosition() is called on fill. We consume the first matching
    // reservation to keep the pending count accurate.
    for (auto it = m_pendingReservations.begin(); it != m_pendingReservations.end(); ++it)
    {
        if (it->second.symbol == symbol)
        {
            m_pendingReservations.erase(it);
            break;
        }
    }

    PULSE_LOG_INFO("risk",
        "Opened {} position {} : {} {} {} @ {:.2f} (lev={:.1f}x, strategy: {})",
        MarketType::Futures == market_type ? "futures" : "spot",
        pos_id, symbol, (Side::Buy == side ? "BUY" : "SELL"),
        qty, entry_price, leverage, strategy_id);

    return pos_id;
}

std::optional<double> PositionManager::closePosition(const std::string &position_id, Quantity close_qty, Price exit_price)
{
    // Close a position fully or partially:
    // 1. Acquire exclusive lock
    // 2. Find position by ID; return nullopt if not found
    // 3. Compute realized PnL for the closed portion
    // 4. If close_qty >= position.quantity: erase (full close)
    // 5. Else: reduce quantity, recalculate notional and unrealized PnL (partial close)
    // 6. Log and return realized PnL

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    auto it = m_positions.find(position_id);
    if (m_positions.end() == it)
    {
        PULSE_LOG_WARN("risk", "closePosition: position {} not found", position_id);
        return std::nullopt;
    }

    auto &pos = it->second;

    // Actual closed quantity (capped at position size for full close).
    const double closed_qty = std::min(close_qty, pos.quantity);

    // Realized PnL for the closed portion.
    //   long:  (exit - entry) * qty * quanto * leverage
    //   short: (entry - exit) * qty * quanto * leverage
    const double direction = (Side::Buy == pos.side) ? 1.0 : -1.0;
    const double realized_pnl =
        direction * (exit_price - pos.entry_price) * closed_qty
        * pos.quanto_multiplier * pos.leverage;

    if (close_qty >= pos.quantity)
    {
        // Full close — remove the position.
        PULSE_LOG_INFO("risk",
            "Closed position {} : {} {} @ {:.2f} (was {} @ {:.2f}), realized PnL={:.4f}",
            position_id, pos.symbol,
            (Side::Buy == pos.side ? "BUY" : "SELL"),
            exit_price, pos.quantity, pos.entry_price, realized_pnl);

        m_positions.erase(it);
    }
    else
    {
        // Partial close — reduce quantity and recalculate.
        pos.quantity -= close_qty;
        pos.current_price = exit_price;
        pos.notional_value = std::abs(pos.quantity * pos.current_price * pos.quanto_multiplier);
        pos.unrealized_pnl = calculateUnrealizedPnl(
            pos.side, pos.entry_price, pos.current_price, pos.quantity,
            pos.leverage, pos.quanto_multiplier);

        // Recalculate margin for remaining quantity.
        if (MarketType::Futures == pos.market_type && pos.leverage > 0.0)
        {
            pos.margin_used = pos.quantity * pos.entry_price * pos.quanto_multiplier / pos.leverage;
        }

        PULSE_LOG_INFO("risk",
            "Partial close position {} : closed {} @ {:.2f}, remaining {} "
            "(unrealized PnL: {:.4f}, realized PnL: {:.4f})",
            position_id, close_qty, exit_price, pos.quantity,
            pos.unrealized_pnl, realized_pnl);
    }

    return realized_pnl;
}

void PositionManager::updatePrice(const std::string &position_id, Price current_price)
{
    // Update mark price for a position:
    // 1. Acquire exclusive lock (modifying position state)
    // 2. Find position; return silently if not found
    // 3. Update current_price, recalculate unrealized_pnl and notional_value

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    auto it = m_positions.find(position_id);
    if (m_positions.end() == it)
    {
        return;
    }

    auto &pos = it->second;
    pos.current_price = current_price;
    pos.unrealized_pnl = calculateUnrealizedPnl(
        pos.side, pos.entry_price, current_price, pos.quantity,
        pos.leverage, pos.quanto_multiplier);
    pos.notional_value = std::abs(pos.quantity * current_price * pos.quanto_multiplier);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Position> PositionManager::getPosition(const std::string &position_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    auto it = m_positions.find(position_id);
    if (m_positions.end() == it)
    {
        return std::nullopt;
    }

    return it->second;
}

std::vector<Position> PositionManager::getAllPositions() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    std::vector<Position> result;
    result.reserve(m_positions.size());
    for (const auto &[id, pos] : m_positions)
    {
        result.push_back(pos);
    }

    return result;
}

std::vector<Position> PositionManager::getPositionsBySymbol(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    std::vector<Position> result;
    for (const auto &[id, pos] : m_positions)
    {
        if (pos.symbol == symbol)
        {
            result.push_back(pos);
        }
    }

    return result;
}

std::vector<Position> PositionManager::getPositionsByStrategy(const std::string &strategy_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    std::vector<Position> result;
    for (const auto &[id, pos] : m_positions)
    {
        if (pos.strategy_id == strategy_id)
        {
            result.push_back(pos);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

PortfolioSummary PositionManager::portfolioSummary() const
{
    // Compute aggregated portfolio snapshot:
    // 1. Acquire shared lock
    // 2. Iterate all positions, accumulate:
    //    - openPositionCount
    //    - total_notional (sum of abs notional values)
    //    - total_unrealized_pnl
    //    - net_exposure (long notional - short notional)

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    PortfolioSummary summary;
    summary.openPositionCount = static_cast<int>(m_positions.size());

    for (const auto &[id, pos] : m_positions)
    {
        summary.total_notional += pos.notional_value;
        summary.total_unrealized_pnl += pos.unrealized_pnl;

        if (Side::Buy == pos.side)
        {
            summary.net_exposure += pos.notional_value;
        }
        else
        {
            summary.net_exposure -= pos.notional_value;
        }

        // Futures-specific aggregation.
        if (MarketType::Futures == pos.market_type)
        {
            summary.total_margin_used += pos.margin_used;
            ++summary.futures_position_count;
        }
    }

    return summary;
}

double PositionManager::symbolNotional(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    double total = 0.0;
    for (const auto &[id, pos] : m_positions)
    {
        if (pos.symbol == symbol)
        {
            total += pos.notional_value;
        }
    }

    return total;
}

// ---------------------------------------------------------------------------
// Limit checks
// ---------------------------------------------------------------------------

bool PositionManager::canOpenPosition(const Symbol &symbol, Quantity qty, Price price) const
{
    // Pre-check whether a new position would exceed any limit:
    // 1. Acquire shared lock
    // 2. Compute proposed notional
    // 3. Check total notional limit
    // 4. Check open position count limit
    // 5. Check per-symbol notional limit
    // 6. Return true only if all checks pass

    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    const double proposed_notional = qty * price;

    // 3. Check total notional limit.
    double total_notional = 0.0;
    for (const auto &[id, pos] : m_positions)
    {
        total_notional += pos.notional_value;
    }

    if (total_notional + proposed_notional > m_config.maxPositionNotional)
    {
        return false;
    }

    // 4. Check open position count.
    if (static_cast<int>(m_positions.size()) >= m_config.maxOpenPositions)
    {
        return false;
    }

    // 5. Check per-symbol notional.
    double sym_notional = 0.0;
    for (const auto &[id, pos] : m_positions)
    {
        if (pos.symbol == symbol)
        {
            sym_notional += pos.notional_value;
        }
    }

    if (sym_notional + proposed_notional > m_config.maxSymbolNotional)
    {
        return false;
    }

    return true;
}

int PositionManager::openPositionCount() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return static_cast<int>(m_positions.size());
}

// ---------------------------------------------------------------------------
// Atomic notional reservation (TOCTOU-safe evaluate + reserve)
// ---------------------------------------------------------------------------

NotionalReservation PositionManager::reserveNotional(
    const Symbol &symbol, Quantity qty, Price price)
{
    // Atomically check all limits and reserve notional budget under a single
    // exclusive lock. This prevents the TOCTOU race where two concurrent
    // evaluateOrder() calls see stale portfolio data and both approve orders
    // that together exceed the limit.
    //
    // The reserved notional is added to m_pendingReservations, so subsequent
    // reserveNotional() calls see it and won't double-spend.

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    NotionalReservation res;
    res.reservation_id = m_nextReservationId++;

    const double proposed_notional = qty * price;

    // Compute current totals under the lock.
    double total_notional = 0.0;
    double sym_notional = 0.0;
    for (const auto &[id, pos] : m_positions)
    {
        total_notional += pos.notional_value;
        if (pos.symbol == symbol)
        {
            sym_notional += pos.notional_value;
        }
    }

    // Add pending reservations to totals (prevents double-spend).
    for (const auto &[id, pend] : m_pendingReservations)
    {
        total_notional += pend.notional;
        if (pend.symbol == symbol)
        {
            sym_notional += pend.notional;
        }
    }

    const int open_count = static_cast<int>(m_positions.size())
                         + static_cast<int>(m_pendingReservations.size());

    // Check if the full order fits within all limits.
    if (total_notional + proposed_notional <= m_config.maxPositionNotional
        && open_count < m_config.maxOpenPositions
        && sym_notional + proposed_notional <= m_config.maxSymbolNotional)
    {
        res.approved = true;
        res.decision = RiskDecision::Approved;
        res.approved_qty = qty;
        res.reserved_notional = proposed_notional;
        res.reason_code = ErrorCode::Ok;
        res.reason_message = "";
    }
    else if (open_count >= m_config.maxOpenPositions)
    {
        // Hard reject: position count limit reached.
        res.approved = false;
        res.decision = RiskDecision::Rejected;
        res.approved_qty = 0.0;
        res.reserved_notional = 0.0;
        res.reason_code = ErrorCode::PositionLimitHit;
        res.reason_message = "Maximum open positions reached";
    }
    else
    {
        // Try reduced quantity.
        const double remaining_pos = m_config.maxPositionNotional - total_notional;
        const double remaining_sym = m_config.maxSymbolNotional - sym_notional;
        const double budget = std::min(remaining_pos, remaining_sym);

        if (budget > 0.0 && price > 0.0)
        {
            const double reduced_qty = budget / price;
            if (reduced_qty > 0.0 && reduced_qty < qty)
            {
                res.approved = true;
                res.decision = RiskDecision::Modified;
                res.approved_qty = reduced_qty;
                res.reserved_notional = reduced_qty * price;
                res.reason_code = ErrorCode::Ok;
                res.reason_message = "Quantity reduced to fit position limit";
            }
            else
            {
                res.approved = false;
                res.decision = RiskDecision::Rejected;
                res.approved_qty = 0.0;
                res.reserved_notional = 0.0;
                res.reason_code = ErrorCode::PositionLimitHit;
                res.reason_message = "No remaining notional budget";
            }
        }
        else
        {
            res.approved = false;
            res.decision = RiskDecision::Rejected;
            res.approved_qty = 0.0;
            res.reserved_notional = 0.0;
            res.reason_code = ErrorCode::PositionLimitHit;
            res.reason_message = "Position notional limit reached";
        }
    }

    // Store the reservation so subsequent checks see it.
    if (res.approved)
    {
        m_pendingReservations[res.reservation_id] = {
            symbol, res.reserved_notional, res.approved_qty };

        PULSE_LOG_INFO("risk",
            "Reserved notional: id={} {} qty={} notional={:.2f} (decision: {})",
            res.reservation_id, symbol, res.approved_qty,
            res.reserved_notional,
            res.decision == RiskDecision::Modified ? "Modified" : "Approved");
    }
    else
    {
        PULSE_LOG_WARN("risk",
            "Reservation rejected: id={} {} reason={}",
            res.reservation_id, symbol, res.reason_message);
    }

    return res;
}

void PositionManager::consumeReservation(std::uint64_t reservation_id)
{
    // Called when an order is filled. Removes the pending reservation so the
    // budget is freed. openPosition() will record the actual position.
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    auto it = m_pendingReservations.find(reservation_id);
    if (it != m_pendingReservations.end())
    {
        PULSE_LOG_DEBUG("risk",
            "Consumed reservation: id={} symbol={} notional={:.2f}",
            reservation_id, it->second.symbol, it->second.notional);
        m_pendingReservations.erase(it);
    }
}

void PositionManager::cancelReservation(std::uint64_t reservation_id)
{
    // Called when an order fails or is rejected after reserveNotional().
    // Releases the reserved budget back to the available pool.
    if (0 == reservation_id)
    {
        return; // No reservation to cancel.
    }

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    auto it = m_pendingReservations.find(reservation_id);
    if (it != m_pendingReservations.end())
    {
        PULSE_LOG_INFO("risk",
            "Cancelled reservation: id={} symbol={} notional={:.2f}",
            reservation_id, it->second.symbol, it->second.notional);
        m_pendingReservations.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string PositionManager::generatePositionId(const Symbol &symbol, Side side)
{
    ++m_nextPositionId;
    return symbol + "_" + (Side::Buy == side ? "Buy" : "Sell") + "_"
        + std::to_string(m_nextPositionId);
}

double PositionManager::calculateUnrealizedPnl(
    Side side, Price entry, Price current, Quantity qty,
    double leverage, double quanto_multiplier)
{
    // Buy: profit when current > entry -> (current - entry) * qty * quanto * leverage
    // Sell: profit when current < entry -> (entry - current) * qty * quanto * leverage
    // For spot: leverage=1.0, quanto_multiplier=1.0 → original formula.
    if (Side::Buy == side)
    {
        return (current - entry) * qty * quanto_multiplier * leverage;
    }
    else
    {
        return (entry - current) * qty * quanto_multiplier * leverage;
    }
}

} // namespace pulse::risk
