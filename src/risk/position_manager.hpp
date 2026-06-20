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

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// NotionalReservation — atomic reservation of notional budget
//
// Created by reserve_notional() when an order is approved. The reservation
// ensures subsequent limit checks see the reserved amount, preventing
// double-spend between concurrent evaluate_order() calls.
//
// Lifecycle:
//   1. reserve_notional() creates a reservation (approved=true)
//   2. On order fill: consume_reservation() frees the reservation
//      (open_position() no longer re-validates the reserved budget)
//   3. On order failure: cancel_reservation() releases the budget
// ---------------------------------------------------------------------------
struct NotionalReservation
{
    std::uint64_t reservation_id; ///< Unique handle for consume/cancel.
    bool approved;                ///< Whether the reservation was granted.
    double approved_qty;          ///< Original or reduced quantity.
    double reserved_notional;     ///< Notional budget reserved.
    RiskDecision decision;        ///< Approved, Modified, or Rejected.
    ErrorCode reason_code;        ///< ErrorCode::Ok if approved.
    std::string reason_message;   ///< Human-readable explanation.

    NotionalReservation()
        : reservation_id{ 0 }
        , approved{ false }
        , approved_qty{ 0.0 }
        , reserved_notional{ 0.0 }
        , decision{ RiskDecision::Rejected }
        , reason_code{ ErrorCode::PositionLimitHit }
        , reason_message{}
    {
    }
};

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

    /// Open a futures position with leverage and margin parameters.
    ///
    /// In addition to standard portfolio limits, computes:
    ///   - margin_used = qty * entry_price * quanto_multiplier / leverage
    ///   - notional = qty * entry_price * quanto_multiplier * leverage
    ///   - liquidation_price (estimated)
    ///
    /// Returns position_id on success, PulseError if any limit is exceeded.
    [[nodiscard]] Result<std::string> open_position(
        const Symbol &symbol, Side side, Quantity qty, Price entry_price,
        const std::string &strategy_id,
        MarketType market_type, double leverage, MarginMode margin_mode,
        double quanto_multiplier, double maintenance_rate);

    /// Close a position (fully or partially).
    ///
    ///   1. If close_qty >= position.quantity: position is removed (full close)
    ///   2. Otherwise: position.quantity is reduced (partial close)
    ///
    /// Returns the realized PnL for the closed portion on success, or
    /// std::nullopt if position_id was not found.
    ///
    /// Realized PnL formula (closed portion):
    ///   long:  (exit_price - entry_price) * closed_qty * quanto * leverage
    ///   short: (entry_price - exit_price) * closed_qty * quanto * leverage
    [[nodiscard]] std::optional<double> close_position(
        const std::string &position_id, Quantity close_qty, Price exit_price);

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

    // --- Atomic notional reservation (TOCTOU-safe) ---

    /// Atomically check all limits and reserve notional budget.
    ///
    /// Performs the same checks as can_open_position() + portfolio_summary() +
    /// symbol_notional(), but under a SINGLE exclusive lock. The reserved
    /// notional is visible to subsequent reserve_notional() calls, preventing
    /// double-spend between concurrent strategy threads.
    ///
    /// Returns a NotionalReservation with a unique reservation_id.
    /// The caller must later call consume_reservation() (on fill) or
    /// cancel_reservation() (on failure) to release the budget.
    [[nodiscard]] NotionalReservation reserve_notional(
        const Symbol &symbol, Quantity qty, Price price);

    /// Consume a reservation when an order is filled.
    /// The reserved budget is released; open_position() trusts the reservation.
    void consume_reservation(std::uint64_t reservation_id);

    /// Cancel a reservation when an order fails or is rejected.
    /// The reserved budget is released back to the available pool.
    void cancel_reservation(std::uint64_t reservation_id);

  private:
    RiskConfig config_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Position> positions_; ///< position_id -> Position.
    std::uint64_t next_position_id_{ 0 };                 ///< Monotonic ID counter.

    /// Pending notional reservations (prevents TOCTOU double-spend).
    struct PendingReservation
    {
        Symbol symbol;
        double notional;
        double qty;
    };
    std::unordered_map<std::uint64_t, PendingReservation> pending_reservations_;
    std::uint64_t next_reservation_id_{ 1 }; ///< Monotonic reservation counter.

    /// Generate a unique position ID from symbol, side, and counter.
    [[nodiscard]] std::string generate_position_id(const Symbol &symbol, Side side);

    /// Calculate unrealized PnL for a position.
    /// Buy: (current - entry) * qty * quanto * leverage
    /// Sell: (entry - current) * qty * quanto * leverage
    /// Default leverage=1.0 and quanto_multiplier=1.0 make spot PnL identical
    /// to the original formula.
    [[nodiscard]] static double calculate_unrealized_pnl(
        Side side, Price entry, Price current, Quantity qty,
        double leverage = 1.0, double quanto_multiplier = 1.0);
};

} // namespace pulse::risk
