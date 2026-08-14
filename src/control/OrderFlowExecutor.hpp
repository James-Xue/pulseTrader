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
#include "risk/DrawdownGuard.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/signal_types.hpp"

#include <cstdint>
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

    /// Signal-aggregator entry point (replaces the main.cpp lambda).
    void onSignal(const strategy::TradingSignal &sig);

    /// Full risk-gated flow for manual orders (REPL/CLI/MCP).
    [[nodiscard]] Result<execution::OrderResponse>
    placeOrder(const execution::OrderRequest &req);

    /// Order-completion entry point (wired to both trackers).
    void onOrderComplete(const execution::ExecutionReport &report);

    /// Cancel an open order; probes futures tracker first, then spot.
    /// Must be called with the shared rest_mutex held.
    [[nodiscard]] bool cancelOrder(const std::string &order_id);

  private:
    /// Build an OrderRequest from a strategy signal (quantity/leverage lookup).
    [[nodiscard]] execution::OrderRequest
    buildRequestFromSignal(const strategy::TradingSignal &sig) const;

    StrategyConfig m_strategyCfg;
    risk::RiskManager &m_riskMgr;
    risk::PositionManager &m_positionMgr;
    risk::DrawdownGuard &m_drawdownGuard;
    IOrderPlacer *m_spotPlacer;
    IOrderPlacer *m_futuresPlacer;
    execution::OrderTracker *m_spotTracker;
    execution::OrderTracker *m_futuresTracker;
    std::mutex &m_restMutex;
#ifdef PULSE_ENABLE_SQLITE
    trade_recorder::TradeRecorder *m_tradeRecorder;
#endif

    std::mutex m_mutex;   ///< Guards m_reservations.
    std::unordered_map<std::string, std::uint64_t> m_reservations;
};

} // namespace pulse::control
