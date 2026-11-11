#pragma once
// sim_account_store.hpp — persistence for the paper-trading (模拟账户) balance
//
// One balance per API key, stored in data/sim_account.json (the data/ dir is
// gitignored). The key is the account identity — same key on the next login
// restores the same simulated balance:
//
//   {
//     "accounts": {
//       "<api key>": { "balance_usdt": 10000.0, "updated_at": "..." }
//     }
//   }
//
// The file is plain JSON for easy inspection; the api key appears only as a
// local identifier (the key itself is already stored in plaintext in .env).
//
// Thread safety: static functions, each opens/closes its own file.

#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace pulse::ui
{

/// One open simulated CFD position (mirrors a real Gate TradFi position:
/// volume in lots, margin = notional / leverage).
struct SimPosition
{
    QString id;                  ///< e.g. "SIM-1700000000000".
    QString side = QStringLiteral("long");  ///< "long" or "short".
    double volumeLots = 0.0;     ///< Lots (XAUUSD min 0.01).
    double entryPrice = 0.0;     ///< Open price.
    double takeProfit = 0.0;     ///< TP price (long: above entry; short: below).
    double stopLoss = 0.0;       ///< SL price (long: below entry; short: above).
    double marginUsdt = 0.0;     ///< Margin locked at open (notional / leverage).
    double feeUsdt = 0.0;        ///< Commission paid at open (BUY side only).
    double leverage = 1.0;       ///< Leverage used for the margin.
    std::int64_t openedAtMs = 0; ///< Open timestamp (Unix ms).
};

/// The persisted per-key simulated account: available balance plus the open
/// positions (margin is deducted from the balance at open, released on close).
struct SimAccount
{
    double balanceUsdt = 0.0;              ///< Available balance.
    QString updatedAt;
    std::vector<SimPosition> positions;    ///< Open positions (persisted).
};

class SimAccountStore
{
  public:

    /// Location of data/sim_account.json: the process working directory's
    /// data/ subdirectory (project root when launched via run.sh or from the
    /// repo), falling back to the executable's directory.
    static QString filePath();

    /// Load the simulated account (balance + positions) bound to `apiKey`.
    /// Returns std::nullopt when the file is missing or the key has no entry.
    static std::optional<SimAccount> load(const QString &apiKey);

    /// Overwrite the balance for `apiKey`, preserving any open positions.
    /// Returns false on write failure (unreadable/missing data dir, etc.).
    static bool save(const QString &apiKey, double balanceUsdt);

    /// Full account write (balance + positions). Returns false on failure.
    static bool saveAccount(const QString &apiKey, const SimAccount &account);

    /// Outcome of a simulated-trade debit.
    struct DebitResult
    {
        bool ok = false;
        double remainingUsdt = 0.0;  ///< Balance after a successful debit.
        QString error;               ///< Human-readable failure reason (Chinese).
    };

    /// Deduct `amountUsdt` from the simulated balance bound to `apiKey`.
    ///
    /// Fails (without touching the store) when the account does not exist
    /// or the balance is insufficient. Used by the strategy engine's paper
    /// trades — real CFD orders are never placed.
    static DebitResult debit(const QString &apiKey, double amountUsdt);
};

} // namespace pulse::ui
