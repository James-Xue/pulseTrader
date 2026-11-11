#pragma once
// sim_trade_engine.hpp — paper-trading engine that mirrors real CFD trading
//
// The simulated account behaves like a real Gate TradFi CFD account:
//   1. Open (long): margin = notional / leverage, commission charged once
//      on the BUY side (0.06 USDT per 0.01 lot — the real CFD fee model),
//      both deducted from the available balance; the position is persisted.
//   2. Close: when the price touches TP (long: price ≥ tp) the position is
//      closed at the TP price; on SL (price ≤ sl) at the SL price. Realized
//      PnL = (exit − entry) × lots × 100 oz/lot, margin is released, the
//      sell side carries no commission (matches the real fee model).
//   3. Insufficient available balance (margin + fee) rejects the open —
//      the same rejection a real account would get.
//
// Storage: the SimAccountStore JSON file (data/sim_account.json), keyed by
// API key — positions survive restarts.
//
// Thread safety: static functions; call them from one thread (the UI thread
// in the launcher). Pure logic — unit-testable without any network.

#include "SimAccountStore.hpp"
#include "SimOrderDb.hpp"

#include <QString>

#include <atomic>
#include <cstdint>
#include <vector>

namespace pulse::ui
{

class SimTradeEngine
{
  public:
    /// XAUUSD contract size: 1 lot = 100 troy ounces (quanto_multiplier=100).
    static constexpr double kOzPerLot = 100.0;
    /// Real CFD commission: 0.06 USDT per 0.01 lot, BUY side only.
    static constexpr double kFeeUsdtPerLot = 6.0;

    struct OpenResult
    {
        bool ok = false;
        QString error;          ///< Human-readable failure reason (Chinese).
        double marginUsdt = 0.0; ///< Margin locked by the position.
        double feeUsdt = 0.0;    ///< Commission charged (BUY side).
        QString positionId;
    };

    struct CloseResult
    {
        bool ok = false;
        QString positionId;
        QString reason;         ///< "TP" or "SL".
        double exitPrice = 0.0;
        double realizedPnl = 0.0;
        double releasedMargin = 0.0;
    };

    /// Open a simulated long position. `leverage` ≥ 1; margin =
    /// lots × 100 × price / leverage; commission = lots × 6 USDT (BUY side).
    /// Requires TP > entry > SL.
    static OpenResult openLong(const QString &apiKey, double volumeLots,
                               double entryPrice, double takeProfit,
                               double stopLoss, double leverage,
                               std::int64_t nowMs);

    /// Open a simulated short position. Same margin model; the sell side
    /// carries no commission (real CFD fee model). Requires TP < entry < SL.
    static OpenResult openShort(const QString &apiKey, double volumeLots,
                                double entryPrice, double takeProfit,
                                double stopLoss, double leverage,
                                std::int64_t nowMs);

    /// Evaluate every open position against `price`; TP/SL hits are closed
    /// and settled immediately (one round of closes, in open order).
    static std::vector<CloseResult> checkAndClose(const QString &apiKey,
                                                  double price,
                                                  std::int64_t nowMs);

    /// Snapshot of the open positions (read-only).
    static std::vector<SimPosition> positions(const QString &apiKey);

    struct StopsUpdateResult
    {
        bool ok = false;
        int updatedCount = 0;   ///< Positions whose stops actually moved.
        QString error;
    };

    /// Apply trailing stops to every open position at `price`.
    ///
    /// Real-CFD behaviour: stops move only in the favourable direction — a
    /// long's TP/SL only move UP (price + tpPoints / price − slPoints), a
    /// short's only move DOWN (price − tpPoints / price + slPoints) — so a
    /// position can only lock in profit, never widen its risk. An
    /// unfavourable move is ignored for that position. Every applied move is
    /// appended to the order_stops history (reason "trailing").
    static StopsUpdateResult updateStops(const QString &apiKey,
                                         double price,
                                         double tpPoints,
                                         double slPoints);
};

} // namespace pulse::ui
