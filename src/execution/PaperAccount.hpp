#pragma once
// PaperAccount.hpp — Simulated account ledger for paper trading (2026-11-25).
//
// This is the venue's money, not the engine's: Liquid cash, locked margin,
// realized PnL and fees are kept per market leg (spot / futures / cfd), which
// mirrors Gate's separate wallets. PositionManager still owns the ENGINE's
// view of positions; PaperAccount owns what the simulated exchange would
// report.
//
// Invariants:
//   1. Equity = free cash + locked margin + unrealized PnL (per leg, then
//      summed). Opening a position moves margin from cash to locked and
//      charges the fee from cash; closing returns margin + gross PnL to cash
//      and charges the close fee.
//   2. No function here reads a clock, generates randomness or touches disk.
//      Every entry point is deterministic for a given call sequence, which is
//      what makes paper runs reproducible and unit-testable.
//   3. Thread safety: one internal mutex. Callers may call from the control
//      socket, the main loop or a tracker thread; no callback is invoked while
//      the lock is held.

#include "core/config.hpp"
#include "core/types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>

namespace pulse::execution
{

/// Wallet legs. The numeric order is part of the persisted schema — do not
/// renumber without a state-file migration.
enum class PaperLeg : std::uint8_t
{
    Spot = 0,
    Futures = 1,
    Cfd = 2,
};

/// One wallet's public view.
struct PaperWalletSnapshot
{
    double cash_usdt{ 0.0 };          ///< Available (unlocked) cash.
    double locked_margin_usdt{ 0.0 }; ///< Margin held by open positions.
    double realized_pnl_usdt{ 0.0 };  ///< Net realized PnL since seeding.
    double fees_paid_usdt{ 0.0 };     ///< Cumulative fees/commissions.
    double unrealized_pnl_usdt{ 0.0 };///< Supplied by the venue (runtime only).
    double margin_free_usdt{ 0.0 };   ///< cash_usdt (alias for clarity).
};

/// Whole-account public view.
struct PaperAccountSnapshot
{
    std::string account_id;
    double equity_usdt{ 0.0 };        ///< Sum of leg equities + unrealized.
    double unrealized_pnl_usdt{ 0.0 };
    double peak_equity_usdt{ 0.0 };
    double daily_start_equity_usdt{ 0.0 };
    PaperWalletSnapshot spot;
    PaperWalletSnapshot futures;
    PaperWalletSnapshot cfd;
};

/// Map a market type onto its wallet leg.
[[nodiscard]] PaperLeg legFor(MarketType market) noexcept;

/// Simulated account ledger. One instance per paper process, shared by the
/// three venue adapters.
class PaperAccount
{
  public:
    /// Seed every leg with `cfg.initial_balance_usdt` and adopt the fee model.
    explicit PaperAccount(const PaperConfig &cfg);

    /// Current snapshot. Unrealized PnL is supplied per leg by the venue (it
    /// owns the marks); the ledger never computes it.
    [[nodiscard]] PaperAccountSnapshot snapshot(double spot_unrealized = 0.0,
                                                double futures_unrealized = 0.0,
                                                double cfd_unrealized = 0.0) const;

    /// Move every leg's locked margin back into free cash.
    ///
    /// Recovery path only: v1 of the state store persists wallets but not
    /// venue positions, so a restart after an open paper position leaves
    /// locked margin with no position behind it. The caller invokes this once
    /// when it starts a venue with an empty book.
    void releaseOrphanedMargin();

    /// True when the leg can pay `amount` of margin + fee.
    [[nodiscard]] bool canAfford(PaperLeg leg, double amount) const;

    /// Move `margin` from free cash to locked margin. Returns false when the
    /// free cash is insufficient; nothing is changed in that case.
    [[nodiscard]] bool lockMargin(PaperLeg leg, double margin);

    /// Return `margin` to free cash (close path). `realized_pnl` is the gross
    /// PnL of the close and is added to both cash and the realized counter.
    void releaseMargin(PaperLeg leg, double margin, double realized_pnl);

    /// Charge a fee from free cash and accumulate the fee counter.
    void chargeFee(PaperLeg leg, double fee);

    /// Credit/debit cash without touching margin (swap, funding, adjustments).
    void adjustCash(PaperLeg leg, double delta);

    /// Track equity high-water marks. `seedPeak` restores them across restart;
    /// the caller passes the persisted values.
    void updateEquityPeak(double equity);
    void seedEquityBaselines(double peak, double daily_start);
    [[nodiscard]] double peakEquity() const;
    [[nodiscard]] double dailyStartEquity() const;

    /// Reset the daily-start baseline (used by the day-boundary hook).
    void resetDailyBaseline(double equity);

    [[nodiscard]] std::string accountId() const;

    /// Serialize / restore the ledger for the state store. The schema is
    /// versioned by the store, not by this object.
    [[nodiscard]] nlohmann::json toJson() const;
    [[nodiscard]] bool restore(const nlohmann::json &j);

  private:
    struct Wallet
    {
        double cash{ 0.0 };
        double locked_margin{ 0.0 };
        double realized_pnl{ 0.0 };
        double fees_paid{ 0.0 };
    };

    [[nodiscard]] Wallet &wallet(PaperLeg leg) noexcept;
    [[nodiscard]] const Wallet &wallet(PaperLeg leg) const noexcept;

    mutable std::mutex m_mutex;
    std::string m_accountId;
    Wallet m_spot;
    Wallet m_futures;
    Wallet m_cfd;
    double m_peakEquity{ 0.0 };
    double m_dailyStartEquity{ 0.0 };
};

} // namespace pulse::execution
