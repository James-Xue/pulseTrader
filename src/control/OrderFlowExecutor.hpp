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
#include "market/OrderBookManager.hpp"
#include "market/SymbolRegistry.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/signal_types.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
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
    ///   7. cfd_placer      — TradFi CFD order placement (nullable if no CFD)
    ///   8. spot_tracker    — spot order tracker (nullable)
    ///   9. futures_tracker — futures order tracker (nullable)
    ///  10. cfd_tracker     — CFD order tracker (nullable)
    ///  11. spot_order_book — live spot order book (nullable; null → maker
    ///                        configs fall back to market orders)
    ///  12. futures_order_book — live futures order book (nullable; same)
    ///  13. rest_mutex      — shared serialization for non-thread-safe REST
    ///  14. trade_recorder  — SQLite recorder (nullable; only when PULSE_ENABLE_SQLITE)
    OrderFlowExecutor(const StrategyConfig &strategy_cfg,
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
    ///
    /// In signal-only mode (config `[strategy] signal_only = true`) this is
    /// a no-op: strategies keep computing signals, but nothing is placed.
    /// Manual orders (placeOrder) are unaffected — they are the sub-agent's
    /// execution path.
    void onSignal(const strategy::TradingSignal &sig);

    /// Whether signal-driven orders are disabled (signal-only mode).
    [[nodiscard]] bool signalOnly() const;

    /// Full risk-gated flow for strategy-originated orders (signal path,
    /// maker-first fallback) and close flows.
    ///
    /// Evaluates the order once, applies a Modified quantity, and places it.
    /// Single-evaluation is important: re-evaluating inside the placement
    /// step would double-count the caller's own notional reservation and
    /// reject every order whose quantity was capped to the budget limit.
    /// Direction gate applies: only the active market may trade unless
    /// reduce_only. Manual orders take placeManualOrder().
    [[nodiscard]] Result<execution::OrderResponse>
    placeOrder(const execution::OrderRequest &req);

    /// Manual-order path (REPL/CLI/MCP) with the M22 relaxed direction gate.
    ///
    /// Futures and CFD orders are executable in any active direction — each
    /// market is bounded by its own notional budget (maxPositionNotional
    /// Futures/Cfd), so the sub-agent can act on signals from both markets
    /// without switching directions. Spot still requires the active
    /// direction (or reduce_only).
    [[nodiscard]] Result<execution::OrderResponse>
    placeManualOrder(const execution::OrderRequest &req);

    /// Order-completion entry point (wired to both trackers).
    void onOrderComplete(const execution::ExecutionReport &report);

    /// Record a TradFi CFD close into the shared record pipeline (completed
    /// reports, drawdown guard, SQLite). The dedicated close endpoint never
    /// emits an order fill, so close_position synthesizes the terminal report
    /// and calls this instead of onOrderComplete — that path would re-close
    /// positions and consume a reservation that never existed.
    void recordCfdClose(const execution::ExecutionReport &report,
                        double pnl, double leverage);

    /// Record an EXTERNAL close (user manual close in the app, or an
    /// exchange-side stop-loss/take-profit fill) into the shared record
    /// pipeline. The engine only learns about these through hot-sync ghost
    /// pruning, so the audit trail (recentReports / trades.db / drawdown
    /// guard / execution.log) would otherwise miss the close side entirely.
    /// market_type picks the tracker; leverage/quanto come from the position.
    void recordExternalClose(const execution::ExecutionReport &report,
                             double pnl, MarketType market_type,
                             double leverage);

    /// Cancel an open order; probes cfd tracker first, then futures, then spot.
    /// Must be called with the shared rest_mutex held.
    [[nodiscard]] bool cancelOrder(const std::string &order_id);

    /// Maker-first sweep: cancel expired post-only attempts and re-issue the
    /// remainder as market orders (fresh risk evaluation). Call every main-loop
    /// iteration WITHOUT holding rest_mutex — the sweep acquires it internally
    /// around the cancel only, and placeOrder() acquires it internally too.
    void sweepMakerAttempts();

    // --- Active trading direction (runtime-switchable) ---
    //
    // Gates strategy-originated orders (signals + maker-first fallback) and
    // spot manual orders: any non-active market is rejected with InactiveMarket
    // (3008) unless reduce_only, so old-direction positions stay closeable
    // after a switch. Futures/CFD manual orders (placeManualOrder) bypass the
    // gate — per-market notional budgets bound them instead.

    /// Set the active trading direction (atomic, lock-free).
    void setActiveMarket(MarketType mt);

    /// Read the active trading direction.
    [[nodiscard]] MarketType activeMarket() const;

    /// Cancel every open order of the given market (reconcile + cancel sweep).
    ///
    /// Bounded to 3 reconcile→cancel passes to catch orders that arrive
    /// concurrently with the switch. Must be called with the shared rest_mutex
    /// held. Returns the number of orders cancelled.
    [[nodiscard]] int cancelAllOpenOrders(MarketType mt);

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
    /// Returns nullopt when the signal must be dropped (post_only config with
    /// no book data — the signal cannot be priced as a maker order).
    [[nodiscard]] std::optional<execution::OrderRequest>
    buildRequestFromSignal(const strategy::TradingSignal &sig) const;

    /// Place an order using a pre-computed risk evaluation. Does NOT
    /// re-evaluate — the reservation from `eval` already holds the budget.
    [[nodiscard]] Result<execution::OrderResponse>
    placeOrder(const execution::OrderRequest &req,
               const risk::RiskEvalResult &eval);

    /// Shared tail of the gated entry points: evaluate exactly once, then
    /// place using that evaluation. Re-evaluating after a Modified result
    /// double-counts the caller's own reservation and rejects orders whose
    /// quantity was capped to the notional budget.
    [[nodiscard]] Result<execution::OrderResponse>
    evaluateAndPlace(const execution::OrderRequest &req);

    /// One in-flight maker-first attempt: the placed post-only request plus
    /// the deadline after which it falls back to a taker order.
    struct MakerAttempt
    {
        std::chrono::steady_clock::time_point deadline;
        execution::OrderRequest request; ///< The request actually placed (post-Modified).
        MarketType market_type;
    };

    /// Locate the strategy instance backing a signal (name match, then first
    /// enabled instance on the symbol). Returns nullptr if none matches.
    [[nodiscard]] const StrategyInstanceConfig *
    matchInstanceConfig(const strategy::TradingSignal &sig) const noexcept;

    /// Best bid (buy) / best ask (sell) from the given book, or nullopt when
    /// the book is null, has no snapshot, or the level set is empty.
    [[nodiscard]] std::optional<Price>
    bestBookPrice(market::OrderBookManager *book, const Symbol &symbol,
                  Side side) const noexcept;

    /// Tracker for a market type (spot/futures/cfd), matching cancelOrder's
    /// probe order. Nullable.
    [[nodiscard]] execution::OrderTracker *
    trackerFor(MarketType mt) const noexcept;

    /// Erase a maker attempt (guarded by m_mutex).
    void eraseAttempt(const std::string &order_id);

    /// Shared SQLite record path for completed trades (onOrderComplete and
    /// recordCfdClose both end here).
    void recordCompletedTrade(const execution::ExecutionReport &report,
                              double pnl, MarketType market_type,
                              double leverage, double quanto_multiplier);

    StrategyConfig m_strategyCfg;
    const bool m_signalOnly; ///< Signal-only mode: onSignal never places orders.
    risk::RiskManager &m_riskMgr;
    risk::PositionManager &m_positionMgr;
    risk::DrawdownGuard &m_drawdownGuard;
    IOrderPlacer *m_spotPlacer;
    IOrderPlacer *m_futuresPlacer;
    IOrderPlacer *m_cfdPlacer;
    execution::OrderTracker *m_spotTracker;
    execution::OrderTracker *m_futuresTracker;
    execution::OrderTracker *m_cfdTracker;
    market::OrderBookManager *m_spotOrderBook;
    market::OrderBookManager *m_futuresOrderBook;
    std::mutex &m_restMutex;
    std::atomic<MarketType> m_activeMarket{ MarketType::Futures };
    std::shared_ptr<const market::SymbolRegistry> m_registry;
#ifdef PULSE_ENABLE_SQLITE
    trade_recorder::TradeRecorder *m_tradeRecorder;
#endif

    std::mutex m_mutex;   ///< Guards m_reservations AND m_makerAttempts.
    std::unordered_map<std::string, ReservationEntry> m_reservations;
    std::unordered_map<std::string, MakerAttempt> m_makerAttempts;
};

} // namespace pulse::control
