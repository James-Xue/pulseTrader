#pragma once
// PaperVenue.hpp — In-process simulated exchange for paper trading (L8).
//
// Why one class implements BOTH interfaces: IOrderPlacer and IOrderTracker must
// share one order/position book. Split objects could let a cancel miss an order
// the poll still fills.
//
// Scope (P2): TradFi CFD only. Futures/spot paper trading is P3.
//
// Two invariants are load-bearing and documented at their enforcement sites:
//   1. place() never fills and never invokes the completion callback — the
//      order-flow reservation is inserted after place() returns.
//   2. A venue-initiated exit is exposed as data (drainExitEvents), never as a
//      direct PositionManager/OrderFlowExecutor call: L9 owns that sequencing.
//
// Layer rule: no market/ or control/ headers. Marks cross the boundary as the
// plain PaperMark struct through an injected provider.

#include "core/config.hpp"
#include "core/types.hpp"
#include "execution/ExecutionReport.hpp"
#include "execution/IOrderPlacer.hpp"
#include "execution/IOrderTracker.hpp"
#include "execution/PaperAccount.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::execution
{

/// One fresh mark, pulled from the market-data layer by the caller's lambda.
struct PaperMark
{
    Price price{ 0.0 };
    std::int64_t timestamp_ms{ 0 };
    Price bid{ 0.0 };
    Price ask{ 0.0 };
};

using PaperMarkProvider =
    std::function<std::optional<PaperMark>(const Symbol &symbol,
                                           MarketType market_type)>;

/// One open venue position, shaped for the control plane.
struct PaperPositionRow
{
    Symbol symbol;
    std::string exchange_position_id;
    Side side{ Side::Buy };
    double lots{ 0.0 };
    double entry_price{ 0.0 };
    double mark_price{ 0.0 };
    double leverage{ 1.0 };
    double locked_margin{ 0.0 };
    double quanto{ 1.0 };
    double sl_price{ 0.0 };
    double tp_price{ 0.0 };
    Timestamp open_time{};
    std::string client_order_id;
};

/// Current protective stops of one venue position.
struct PaperStopState
{
    double sl_price{ 0.0 };
    double tp_price{ 0.0 };
};

/// Result of an engine-initiated CFD close.
struct PaperCfdClose
{
    double exit_price{ 0.0 };
    double realized_pnl{ 0.0 };
    double margin_released{ 0.0 };
    double fee{ 0.0 };
};

enum class PaperExitReason : std::uint8_t
{
    StopLoss,
    TakeProfit,
    StopOut,
};

/// A venue-initiated exit, drained by L9 (tickPaperVenue).
///
/// Venue truth: by the time this event exists the venue has already applied the
/// ledger close (realized PnL, margin release, commission), and `tick()` erases
/// the position immediately afterwards. Anything the trade ledger or the GT
/// bridge needs must therefore be captured here — recomputing it downstream
/// reads state that no longer exists (2026-11-27 reconciliation audit, P1).
struct PaperExitEvent
{
    std::string exchange_position_id;
    Symbol symbol;
    Side position_side{ Side::Buy };
    double closed_qty{ 0.0 };
    double fill_price{ 0.0 };
    double fee{ 0.0 };
    Timestamp fill_time{};
    PaperExitReason reason{ PaperExitReason::StopLoss };

    // Venue-truth economics, snapshotted before the position is erased.
    double realized_pnl{ 0.0 };  ///< Already booked into PaperAccount.
    double entry_price{ 0.0 };   ///< Position entry, for the ledger row.
    std::string client_order_id; ///< Member/strategy tag of the entry order.
    MarketType market_type{ MarketType::Cfd }; ///< PaperVenue is CFD-only.
    double leverage{ 1.0 };
    double quanto{ 1.0 };        ///< Contract multiplier (100 for 0.01-lot XAUUSD).
};

class PaperVenue final : public IOrderPlacer, public IOrderTracker
{
  public:
    /// Parameters:
    ///   1. cfg      — [paper] model (fees, tick, stop-out, mark age)
    ///   2. symbols  — symbols this venue will accept orders for
    ///   3. account  — shared ledger; read through the reference on every query
    ///                 (the caller may restore it after construction)
    PaperVenue(const PaperConfig &cfg, std::vector<Symbol> symbols,
               PaperAccount &account);

    // ------------------------------------------------------------------
    // IOrderPlacer
    // ------------------------------------------------------------------

    [[nodiscard]] Result<OrderResponse>
    place(const OrderRequest &req) override;

    [[nodiscard]] bool cancel(const std::string &order_id) override;

    [[nodiscard]] Result<nlohmann::json>
    setLeverage(const std::string &contract, double leverage) override;

    // ------------------------------------------------------------------
    // IOrderTracker
    // ------------------------------------------------------------------

    void trackOrder(const std::string &order_id,
        const Symbol &symbol,
        Side side,
        OrderType type,
        Quantity requested_qty,
        Price submit_mid_price,
        const std::string &client_order_id = "") override;

    [[nodiscard]] std::optional<OrderStatus>
    getStatus(const std::string &order_id) const override;

    void setCompletionCallback(CompletionCallback callback) override;

    [[nodiscard]] Result<OrderStatus>
    pollOrderStatus(const std::string &order_id) override;

    void reconcileAll() override;

    /// Deliberately ignores the mutex: the venue is in-process, and
    /// std::mutex is not recursive (the caller may already hold it).
    void reconcileAll(std::mutex &rest_mutex) override;

    [[nodiscard]] std::vector<OrderSnapshot> activeOrders() const override;

    [[nodiscard]] std::vector<ExecutionReport>
    recentReports(std::size_t n = 20) const override;

    void recordCompletedReport(const ExecutionReport &report) override;

    [[nodiscard]] std::string
    resolveExchangeId(const std::string &order_id) const override;

    // ------------------------------------------------------------------
    // Venue surface (consumed by L9)
    // ------------------------------------------------------------------

    /// Install the mark source. Call once at startup, before any order.
    void setMarkProvider(PaperMarkProvider provider);

    /// Evaluate resting orders, protective stops and stop-out.
    /// `now_ms` is a parameter so tests are deterministic.
    void tick(std::int64_t now_ms);

    [[nodiscard]] std::optional<PaperMark>
    markFor(const Symbol &symbol, MarketType market_type) const;

    [[nodiscard]] double freeMargin(PaperLeg leg) const;

    [[nodiscard]] double unrealizedPnl(PaperLeg leg) const;

    [[nodiscard]] double unrealizedPnlTotal() const;

    [[nodiscard]] PaperAccountSnapshot accountSnapshot() const;

    [[nodiscard]] std::vector<PaperPositionRow>
    openPositions(MarketType market_type) const;

    [[nodiscard]] std::optional<PaperStopState>
    cfdStopState(const std::string &exchange_position_id) const;

    /// Engine-initiated close (control-plane close_position). Applies the
    /// ledger close and returns the fill; it does NOT enqueue an exit event
    /// (one close, one sink — L9 records it via recordCfdClose).
    [[nodiscard]] Result<PaperCfdClose>
    closeCfdPosition(const std::string &exchange_position_id, double lots,
                     double requested_qty_total);

    [[nodiscard]] Result<nlohmann::json>
    modifyCfdStops(const std::string &exchange_position_id, double sl_price,
                   double tp_price);

    /// Drain venue-initiated exits (SL/TP/stop-out). L9 closes PositionManager
    /// first, then records.
    [[nodiscard]] std::vector<PaperExitEvent> drainExitEvents();

    /// Number of open venue positions (diagnostics/tests).
    [[nodiscard]] std::size_t openPositionCount() const;

    /// Serialize the venue book (positions + id sequence) for the state
    /// store. The account (cash/margin) lives in PaperAccount and is
    /// persisted separately in the same file.
    [[nodiscard]] nlohmann::json toJson() const;

    /// Rebuild the venue book from a previous toJson(). Returns false on a
    /// structurally invalid payload (the caller must then fail closed).
    [[nodiscard]] bool restore(const nlohmann::json &j);

  private:
    struct PendingOrder
    {
        std::string order_id;
        std::string client_order_id;
        Symbol symbol;
        Side side{ Side::Buy };
        OrderType type{ OrderType::Market };
        double requested_qty{ 0.0 };
        double limit_price{ 0.0 };
        double submit_mid_price{ 0.0 };
        double filled_qty{ 0.0 };
        double avg_fill_price{ 0.0 };
        double fees{ 0.0 };
        double leverage{ 1.0 };
        double quanto{ 1.0 };
        double sl_price{ 0.0 };
        double tp_price{ 0.0 };
        OrderStatus status{ OrderStatus::Pending };
        Timestamp submit_time{};
        Timestamp last_update_time{};
        std::string exchange_position_id;
        bool terminal{ false };
    };

    struct PaperPosition
    {
        std::string exchange_position_id;
        Symbol symbol;
        Side side{ Side::Buy };
        double lots{ 0.0 };
        double entry_price{ 0.0 };
        double leverage{ 1.0 };
        double quanto{ 1.0 };
        double locked_margin{ 0.0 };
        double sl_price{ 0.0 };
        double tp_price{ 0.0 };
        Timestamp open_time{};
        std::string client_order_id;
    };

    /// Fresh mark or nullopt when the provider is absent, the quote is
    /// missing, or it is older than [paper] max_mark_age_ms.
    /// Caller must NOT hold m_mutex.
    [[nodiscard]] std::optional<PaperMark>
    freshMark(const Symbol &symbol, MarketType market_type,
              std::int64_t now_ms) const;

    /// Same check, for callers that ALREADY hold m_mutex (std::mutex is not
    /// recursive, so this must not re-lock).
    [[nodiscard]] std::optional<PaperMark>
    freshMarkLocked(const Symbol &symbol, MarketType market_type,
                    std::int64_t now_ms) const;

    [[nodiscard]] double quantoFor(const Symbol &symbol) const;

    [[nodiscard]] double commissionFor(Side side, double lots) const;

    /// Build the terminal report for a filled order (caller holds the lock).
    [[nodiscard]] ExecutionReport buildFillReport(PendingOrder &order,
                                                  double fill_price,
                                                  Timestamp now) const;

    /// Fill a market order at the current mark. Called only from
    /// pollOrderStatus (never from place). Returns the new status.
    [[nodiscard]] Result<OrderStatus>
    fillMarketOrder(PendingOrder &order, std::int64_t now_ms);

    /// Convert an order into a venue position (caller holds the lock).
    void openPositionFromOrder(PendingOrder &order, double fill_price);

    /// Cancel an order without a fill (caller holds the lock).
    void cancelOrderLocked(PendingOrder &order, Timestamp now);

    /// Close a venue position at `fill_price` (caller holds the lock).
    /// Returns realized PnL and releases proportional margin.
    double closePositionLocked(PaperPosition &position, double lots,
                               double fill_price, Timestamp now,
                               bool emit_exit, PaperExitReason reason);

    const PaperConfig m_cfg;
    PaperAccount &m_account;
    std::vector<Symbol> m_symbols;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, PendingOrder> m_orders;
    std::vector<PaperPosition> m_positions;
    std::vector<ExecutionReport> m_reports;
    std::vector<PaperExitEvent> m_exits;
    CompletionCallback m_completion;
    PaperMarkProvider m_markProvider;
    std::uint64_t m_seq{ 0 };
};

} // namespace pulse::execution
