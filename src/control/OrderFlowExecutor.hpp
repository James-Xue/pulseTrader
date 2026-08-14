#pragma once
// OrderFlowExecutor.hpp — Risk-gated order execution flow (control plane)
//
// Extracted from apps/pulsetrader/main.cpp: owns the notional-reservation
// map (order_id → reservation_id) so BOTH the signal aggregator path and
// the manual CLI/MCP path share one consistent flow:
//
//   evaluateOrder → (Modified: reduce qty) → placeOrder (under rest_mutex)
//                → record reservation → trackOrder
//
// Order completion (WS/REST poll) consumes the reservation, opens/closes
// positions, updates the drawdown guard, and records the trade in SQLite.

#include "core/PulseError.hpp"
#include "core/config.hpp"
#include "execution/OrderExecutor.hpp"
#include "execution/OrderTracker.hpp"
#include "market/SymbolRegistry.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/signal_types.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef PULSE_ENABLE_SQLITE
#include "trade_recorder/TradeRecorder.hpp"
#endif

namespace pulse::control
{

// ---------------------------------------------------------------------------
// IOrderPlacer — thin interface so unit tests can fake order placement
// (OrderExecutor is concrete, takes a live GateRestClient, and is not virtual)
// ---------------------------------------------------------------------------
class IOrderPlacer
{
  public:
    virtual ~IOrderPlacer() = default;

    /// Place an order via the exchange REST API.
    [[nodiscard]] virtual Result<execution::OrderResponse>
    place(const execution::OrderRequest &req) = 0;

    /// Cancel an order by exchange order ID.
    [[nodiscard]] virtual bool cancel(const std::string &order_id) = 0;

    /// Ensure futures leverage matches the request before an order is placed.
    /// Gate.io applies leverage at the position level, not per order — the
    /// account's current setting (possibly 200x) silently applies otherwise.
    /// Spot placers implement as a no-op.
    [[nodiscard]] virtual Result<nlohmann::json>
    setLeverage(const std::string &contract, double leverage) = 0;
};

/// IOrderPlacer backed by a real OrderExecutor.
class ExecutorOrderPlacer final : public IOrderPlacer
{
  public:
    explicit ExecutorOrderPlacer(execution::OrderExecutor &exec)
        : m_exec{ exec }
    {
    }

    [[nodiscard]] Result<execution::OrderResponse>
    place(const execution::OrderRequest &req) override;

    [[nodiscard]] bool cancel(const std::string &order_id) override;

    [[nodiscard]] Result<nlohmann::json>
    setLeverage(const std::string &contract, double leverage) override;

  private:
    execution::OrderExecutor &m_exec;
};

// ---------------------------------------------------------------------------
// OrderFlowExecutor
// ---------------------------------------------------------------------------
class OrderFlowExecutor
{
  public:
    /// Parameters:
    ///   1. strategy_cfg    — strategy config (quantity/leverage lookup by signal)
    ///   2. risk_mgr        — risk evaluation facade
    ///   3. position_mgr    — position bookkeeping (reservations, open/close)
    ///   4. drawdown_guard  — realized-PnL sink
    ///   5. spot_placer     — spot order placement (nullable if no spot)
    ///   6. futures_placer  — futures order placement (nullable if no futures)
    ///   7. spot_tracker    — spot order tracker (nullable)
    ///   8. futures_tracker — futures order tracker (nullable)
    ///   9. rest_mutex      — shared serialization for non-thread-safe REST
    ///  10. trade_recorder  — SQLite recorder (nullable; only when PULSE_ENABLE_SQLITE)
    OrderFlowExecutor(const StrategyConfig &strategy_cfg,
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
                      int /*trade_recorder*/
#endif
    );

    /// Attach instrument metadata (contract multipliers etc.). Fetched once at
    /// startup from the exchange REST API and shared read-only afterwards.
    /// Safe to call before any signal/manual order is processed.
    void setSymbolRegistry(std::shared_ptr<const market::SymbolRegistry> registry);

    /// Contract multiplier for a symbol (1.0 if unknown or spot).
    [[nodiscard]] double quantoMultiplierFor(const Symbol &symbol) const;

    /// Signal-aggregator entry point (replaces the main.cpp lambda).
    void onSignal(const strategy::TradingSignal &sig);

    /// Full risk-gated flow for manual orders (REPL/CLI/MCP).
    ///
    /// Evaluates the order once, applies a Modified quantity, and places it.
    /// Single-evaluation is important: re-evaluating inside the placement
    /// step would double-count the caller's own notional reservation and
    /// reject every order whose quantity was capped to the budget limit.
    [[nodiscard]] Result<execution::OrderResponse>
    placeOrder(const execution::OrderRequest &req);

    /// Order-completion entry point (wired to both trackers).
    void onOrderComplete(const execution::ExecutionReport &report);

    /// Cancel an open order; probes futures tracker first, then spot.
    /// Must be called with the shared rest_mutex held.
    [[nodiscard]] bool cancelOrder(const std::string &order_id);

  private:
    /// Reservation bookkeeping: maps a placed exchange order_id to the
    /// reservation that holds its notional budget, plus the request that was
    /// placed (needed on fill to open the position with the correct market
    /// type, leverage, and contract multiplier).
    struct ReservationEntry
    {
        std::uint64_t reservation_id{ 0 };
        execution::OrderRequest request;
    };

    /// Build an OrderRequest from a strategy signal (quantity/leverage lookup).
    [[nodiscard]] execution::OrderRequest
    buildRequestFromSignal(const strategy::TradingSignal &sig) const;

    /// Place an order using a pre-computed risk evaluation. Does NOT
    /// re-evaluate — the reservation from `eval` already holds the budget.
    [[nodiscard]] Result<execution::OrderResponse>
    placeOrder(const execution::OrderRequest &req,
               const risk::RiskEvalResult &eval);

    StrategyConfig m_strategyCfg;
    risk::RiskManager &m_riskMgr;
    risk::PositionManager &m_positionMgr;
    risk::DrawdownGuard &m_drawdownGuard;
    IOrderPlacer *m_spotPlacer;
    IOrderPlacer *m_futuresPlacer;
    execution::OrderTracker *m_spotTracker;
    execution::OrderTracker *m_futuresTracker;
    std::mutex &m_restMutex;
    std::shared_ptr<const market::SymbolRegistry> m_registry;
#ifdef PULSE_ENABLE_SQLITE
    trade_recorder::TradeRecorder *m_tradeRecorder;
#endif

    std::mutex m_mutex;   ///< Guards m_reservations.
    std::unordered_map<std::string, ReservationEntry> m_reservations;
};

} // namespace pulse::control
