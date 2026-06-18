#pragma once
// position_manager.hpp — Thread-safe position tracking (Layer 7 Risk Management)
//
// Tracks open positions across all strategies in real time. Aggregates net
// exposure per symbol and enforces portfolio-level limits:
//   1. Max total notional (maxPositionNotional)
//   2. Max open positions (maxOpenPositions)
//   3. Max per-symbol notional (maxSymbolNotional)
//
// Thread safety:
//   - std::shared_mutex (reader-writer lock)
//   - Multiple strategy threads read concurrently (shared lock)
//   - Writes (open/close/update) take exclusive lock
//   - Query methods are const and take shared lock

#include "core/config.hpp"
#include "core/error.hpp"
#include "core/types.hpp"
#include "risk/risk_types.hpp"

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// PositionManager — tracks open positions and enforces portfolio limits
// ---------------------------------------------------------------------------
class PositionManager
{
  public:
    /// Construct with risk configuration containing position limits.
    explicit PositionManager(const RiskConfig &config);

    // --- Position lifecycle ---

    /// Open a new position.
    ///
    /// Validates against portfolio limits before creating:
    ///   1. Check total notional + proposed notional <= maxPositionNotional
    ///   2. Check open position count < maxOpenPositions
    ///   3. Check per-symbol notional + proposed <= maxSymbolNotional
    ///
    /// Returns position_id on success, PulseError if any limit is exceeded.
    [[nodiscard]] Result<std::string> open_position(
        const Symbol &symbol, Side side, Quantity qty, Price entry_price,
        const std::string &strategy_id);

    /// Close a position (fully or partially).
    ///
    ///   1. If close_qty >= position.quantity: position is removed (full close)
    ///   2. Otherwise: position.quantity is reduced (partial close)
    ///
    /// Returns true on success, false if position_id not found.
    [[nodiscard]] bool close_position(const std::string &position_id, Quantity close_qty, Price exit_price);

    /// Update mark price for a position (called on each tick for PnL recalculation).
    ///
    /// Recalculates:
    ///   1. current_price = new price
    ///   2. unrealized_pnl = (current - entry) * qty * direction
    ///   3. notional_value = abs(qty * current_price)
    void update_price(const std::string &position_id, Price current_price);

    // --- Queries ---

    /// Get a single position by ID. Returns nullopt if not found.
    [[nodiscard]] std::optional<Position> get_position(const std::string &position_id) const;

    /// Get all open positions.
    [[nodiscard]] std::vector<Position> get_all_positions() const;

    /// Get all open positions for a specific symbol.
    [[nodiscard]] std::vector<Position> get_positions_by_symbol(const Symbol &symbol) const;

    /// Get all open positions for a specific strategy.
    [[nodiscard]] std::vector<Position> get_positions_by_strategy(const std::string &strategy_id) const;

    // --- Aggregation ---

    /// Compute aggregated portfolio summary from all open positions.
    [[nodiscard]] PortfolioSummary portfolio_summary() const;

    /// Compute total notional exposure for a specific symbol.
    [[nodiscard]] double symbol_notional(const Symbol &symbol) const;

    // --- Limit checks ---

    /// Check whether a new position can be opened without exceeding any limit.
    ///
    /// Evaluates:
    ///   1. Total notional + proposed <= maxPositionNotional
    ///   2. Open count < maxOpenPositions
    ///   3. Symbol notional + proposed <= maxSymbolNotional
    [[nodiscard]] bool can_open_position(const Symbol &symbol, Quantity qty, Price price) const;

    /// Returns the current number of open positions.
    [[nodiscard]] int open_position_count() const;

  private:
    RiskConfig config_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Position> positions_; ///< position_id -> Position.
    std::uint64_t next_position_id_{ 0 };                 ///< Monotonic ID counter.

    /// Generate a unique position ID from symbol, side, and counter.
    [[nodiscard]] std::string generate_position_id(const Symbol &symbol, Side side);

    /// Calculate unrealized PnL for a position.
    /// Buy: (current - entry) * qty; Sell: (entry - current) * qty.
    [[nodiscard]] static double calculate_unrealized_pnl(Side side, Price entry, Price current, Quantity qty);
};

} // namespace pulse::risk
