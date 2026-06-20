// position_manager.cpp — Thread-safe position tracking (Layer 7 Risk Management)

#include "risk/position_manager.hpp"

#include "logging/logger.hpp"

#include <algorithm>
#include <cmath>

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PositionManager::PositionManager(const RiskConfig &config)
    : config_{ config }
{
}

// ---------------------------------------------------------------------------
// Position lifecycle
// ---------------------------------------------------------------------------

Result<std::string> PositionManager::open_position(
    const Symbol &symbol, Side side, Quantity qty, Price entry_price,
    const std::string &strategy_id)
{
    // Delegate to the full overload with spot defaults.
    return open_position(symbol, side, qty, entry_price, strategy_id,
                         MarketType::Spot, 1.0, MarginMode::Cross, 1.0, 0.0);
}

Result<std::string> PositionManager::open_position(
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

    std::unique_lock<std::shared_mutex> write_lock(mutex_);

    const double proposed_notional = qty * entry_price * quanto_multiplier;

    // 3. Check total notional limit.
    double total_notional = 0.0;
    for (const auto &[id, pos] : positions_)
    {
        total_notional += pos.notional_value;
    }

    if (total_notional + proposed_notional > config_.maxPositionNotional)
    {
        PULSE_LOG_WARN("risk",
            "Position rejected: total notional {:.2f} + proposed {:.2f} exceeds limit {:.2f}",
            total_notional, proposed_notional, config_.maxPositionNotional);
        return PulseError{ ErrorCode::PositionLimitHit,
            "Total notional limit exceeded" };
    }

    // 4. Check open position count limit.
    if (static_cast<int>(positions_.size()) >= config_.maxOpenPositions)
    {
        PULSE_LOG_WARN("risk",
            "Position rejected: {} open positions >= limit {}",
            positions_.size(), config_.maxOpenPositions);
        return PulseError{ ErrorCode::PositionLimitHit,
            "Maximum open positions reached" };
    }

    // 5. Check per-symbol notional limit.
    double sym_notional = 0.0;
    for (const auto &[id, pos] : positions_)
    {
        if (pos.symbol == symbol)
        {
            sym_notional += pos.notional_value;
        }
    }

    if (sym_notional + proposed_notional > config_.maxSymbolNotional)
    {
        PULSE_LOG_WARN("risk",
            "Position rejected: symbol {} notional {:.2f} + proposed {:.2f} exceeds limit {:.2f}",
            symbol, sym_notional, proposed_notional, config_.maxSymbolNotional);
        return PulseError{ ErrorCode::SymbolLimitHit,
            "Per-symbol notional limit exceeded" };
    }

    // 6. Generate unique position ID.
    std::string pos_id = generate_position_id(symbol, side);

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

    positions_[pos_id] = pos;

    PULSE_LOG_INFO("risk",
        "Opened {} position {} : {} {} {} @ {:.2f} (lev={:.1f}x, strategy: {})",
        MarketType::Futures == market_type ? "futures" : "spot",
        pos_id, symbol, (Side::Buy == side ? "BUY" : "SELL"),
        qty, entry_price, leverage, strategy_id);

    return pos_id;
}

bool PositionManager::close_position(const std::string &position_id, Quantity close_qty, Price exit_price)
{
    // Close a position fully or partially:
    // 1. Acquire exclusive lock
    // 2. Find position by ID; return false if not found
    // 3. If close_qty >= position.quantity: erase (full close)
    // 4. Else: reduce quantity, recalculate notional and PnL (partial close)
    // 5. Log and return true

    std::unique_lock<std::shared_mutex> write_lock(mutex_);

    auto it = positions_.find(position_id);
    if (positions_.end() == it)
    {
        PULSE_LOG_WARN("risk", "close_position: position {} not found", position_id);
        return false;
    }

    auto &pos = it->second;

    if (close_qty >= pos.quantity)
    {
        // Full close — remove the position.
        PULSE_LOG_INFO("risk",
            "Closed position {} : {} {} @ {:.2f} (was {} @ {:.2f})",
            position_id, pos.symbol,
            (Side::Buy == pos.side ? "BUY" : "SELL"),
            exit_price, pos.quantity, pos.entry_price);

        positions_.erase(it);
    }
    else
    {
        // Partial close — reduce quantity and recalculate.
        pos.quantity -= close_qty;
        pos.current_price = exit_price;
        pos.notional_value = std::abs(pos.quantity * pos.current_price * pos.quanto_multiplier);
        pos.unrealized_pnl = calculate_unrealized_pnl(
            pos.side, pos.entry_price, pos.current_price, pos.quantity,
            pos.leverage, pos.quanto_multiplier);

        // Recalculate margin for remaining quantity.
        if (MarketType::Futures == pos.market_type && pos.leverage > 0.0)
        {
            pos.margin_used = pos.quantity * pos.entry_price * pos.quanto_multiplier / pos.leverage;
        }

        PULSE_LOG_INFO("risk",
            "Partial close position {} : closed {} @ {:.2f}, remaining {} (PnL: {:.4f})",
            position_id, close_qty, exit_price, pos.quantity, pos.unrealized_pnl);
    }

    return true;
}

void PositionManager::update_price(const std::string &position_id, Price current_price)
{
    // Update mark price for a position:
    // 1. Acquire exclusive lock (modifying position state)
    // 2. Find position; return silently if not found
    // 3. Update current_price, recalculate unrealized_pnl and notional_value

    std::unique_lock<std::shared_mutex> write_lock(mutex_);

    auto it = positions_.find(position_id);
    if (positions_.end() == it)
    {
        return;
    }

    auto &pos = it->second;
    pos.current_price = current_price;
    pos.unrealized_pnl = calculate_unrealized_pnl(
        pos.side, pos.entry_price, current_price, pos.quantity,
        pos.leverage, pos.quanto_multiplier);
    pos.notional_value = std::abs(pos.quantity * current_price * pos.quanto_multiplier);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

std::optional<Position> PositionManager::get_position(const std::string &position_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    auto it = positions_.find(position_id);
    if (positions_.end() == it)
    {
        return std::nullopt;
    }

    return it->second;
}

std::vector<Position> PositionManager::get_all_positions() const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    std::vector<Position> result;
    result.reserve(positions_.size());
    for (const auto &[id, pos] : positions_)
    {
        result.push_back(pos);
    }

    return result;
}

std::vector<Position> PositionManager::get_positions_by_symbol(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    std::vector<Position> result;
    for (const auto &[id, pos] : positions_)
    {
        if (pos.symbol == symbol)
        {
            result.push_back(pos);
        }
    }

    return result;
}

std::vector<Position> PositionManager::get_positions_by_strategy(const std::string &strategy_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    std::vector<Position> result;
    for (const auto &[id, pos] : positions_)
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

PortfolioSummary PositionManager::portfolio_summary() const
{
    // Compute aggregated portfolio snapshot:
    // 1. Acquire shared lock
    // 2. Iterate all positions, accumulate:
    //    - open_position_count
    //    - total_notional (sum of abs notional values)
    //    - total_unrealized_pnl
    //    - net_exposure (long notional - short notional)

    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    PortfolioSummary summary;
    summary.open_position_count = static_cast<int>(positions_.size());

    for (const auto &[id, pos] : positions_)
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

double PositionManager::symbol_notional(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    double total = 0.0;
    for (const auto &[id, pos] : positions_)
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

bool PositionManager::can_open_position(const Symbol &symbol, Quantity qty, Price price) const
{
    // Pre-check whether a new position would exceed any limit:
    // 1. Acquire shared lock
    // 2. Compute proposed notional
    // 3. Check total notional limit
    // 4. Check open position count limit
    // 5. Check per-symbol notional limit
    // 6. Return true only if all checks pass

    std::shared_lock<std::shared_mutex> read_lock(mutex_);

    const double proposed_notional = qty * price;

    // 3. Check total notional limit.
    double total_notional = 0.0;
    for (const auto &[id, pos] : positions_)
    {
        total_notional += pos.notional_value;
    }

    if (total_notional + proposed_notional > config_.maxPositionNotional)
    {
        return false;
    }

    // 4. Check open position count.
    if (static_cast<int>(positions_.size()) >= config_.maxOpenPositions)
    {
        return false;
    }

    // 5. Check per-symbol notional.
    double sym_notional = 0.0;
    for (const auto &[id, pos] : positions_)
    {
        if (pos.symbol == symbol)
        {
            sym_notional += pos.notional_value;
        }
    }

    if (sym_notional + proposed_notional > config_.maxSymbolNotional)
    {
        return false;
    }

    return true;
}

int PositionManager::open_position_count() const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    return static_cast<int>(positions_.size());
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string PositionManager::generate_position_id(const Symbol &symbol, Side side)
{
    ++next_position_id_;
    return symbol + "_" + (Side::Buy == side ? "Buy" : "Sell") + "_"
        + std::to_string(next_position_id_);
}

double PositionManager::calculate_unrealized_pnl(
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
