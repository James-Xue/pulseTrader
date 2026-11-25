// paper_account.cpp — Simulated account ledger (2026-11-25).

#include "execution/PaperAccount.hpp"

#include <algorithm>
#include <array>

namespace pulse::execution
{

PaperLeg legFor(MarketType market) noexcept
{
    switch (market)
    {
    case MarketType::Futures:
        return PaperLeg::Futures;
    case MarketType::Cfd:
        return PaperLeg::Cfd;
    default:
        return PaperLeg::Spot;
    }
}

PaperAccount::PaperAccount(const PaperConfig &cfg)
    : m_accountId{ cfg.account_id }
{
    // Every leg starts with the same seeded balance, mirroring Gate's
    // independent wallets rather than splitting one total (see the design
    // note in docs/paper-trading.md).
    m_spot.cash = cfg.initial_balance_usdt;
    m_futures.cash = cfg.initial_balance_usdt;
    m_cfd.cash = cfg.initial_balance_usdt;

    const double seeded_total = cfg.initial_balance_usdt * 3.0;
    m_peakEquity = seeded_total;
    m_dailyStartEquity = seeded_total;
}

PaperAccount::Wallet &PaperAccount::wallet(PaperLeg leg) noexcept
{
    switch (leg)
    {
    case PaperLeg::Futures:
        return m_futures;
    case PaperLeg::Cfd:
        return m_cfd;
    default:
        return m_spot;
    }
}

const PaperAccount::Wallet &PaperAccount::wallet(PaperLeg leg) const noexcept
{
    switch (leg)
    {
    case PaperLeg::Futures:
        return m_futures;
    case PaperLeg::Cfd:
        return m_cfd;
    default:
        return m_spot;
    }
}

namespace
{
PaperWalletSnapshot snapshotOf(const auto &w)
{
    PaperWalletSnapshot s;
    s.cash_usdt = w.cash;
    s.locked_margin_usdt = w.locked_margin;
    s.realized_pnl_usdt = w.realized_pnl;
    s.fees_paid_usdt = w.fees_paid;
    s.margin_free_usdt = w.cash;
    return s;
}
} // namespace

PaperAccountSnapshot PaperAccount::snapshot(double unrealized_pnl) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    PaperAccountSnapshot s;
    s.account_id = m_accountId;
    s.unrealized_pnl_usdt = unrealized_pnl;
    s.spot = snapshotOf(m_spot);
    s.futures = snapshotOf(m_futures);
    s.cfd = snapshotOf(m_cfd);
    s.equity_usdt = m_spot.cash + m_spot.locked_margin + m_futures.cash
        + m_futures.locked_margin + m_cfd.cash + m_cfd.locked_margin
        + unrealized_pnl;
    s.peak_equity_usdt = m_peakEquity;
    s.daily_start_equity_usdt = m_dailyStartEquity;
    return s;
}

bool PaperAccount::canAfford(PaperLeg leg, double amount) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // A small epsilon keeps a fee-only order affordable when the balance is
    // exactly the requirement (binary floating point).
    return wallet(leg).cash + 1e-9 >= amount;
}

bool PaperAccount::lockMargin(PaperLeg leg, double margin)
{
    if (margin < 0.0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    Wallet &w = wallet(leg);
    if (w.cash + 1e-9 < margin)
    {
        return false;
    }
    w.cash -= margin;
    w.locked_margin += margin;
    return true;
}

void PaperAccount::releaseMargin(PaperLeg leg, double margin,
                                 double realized_pnl)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Wallet &w = wallet(leg);
    // Never release more margin than is actually locked: a mismatch means a
    // ledger bug, and silently minting cash would hide it.
    const double released = std::min(margin, w.locked_margin);
    w.locked_margin -= released;
    w.cash += released + realized_pnl;
    w.realized_pnl += realized_pnl;
}

void PaperAccount::chargeFee(PaperLeg leg, double fee)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Wallet &w = wallet(leg);
    w.cash -= fee;
    w.fees_paid += fee;
}

void PaperAccount::adjustCash(PaperLeg leg, double delta)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    wallet(leg).cash += delta;
}

void PaperAccount::updateEquityPeak(double equity)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_peakEquity = std::max(m_peakEquity, equity);
}

void PaperAccount::seedEquityBaselines(double peak, double daily_start)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_peakEquity = peak;
    m_dailyStartEquity = daily_start;
}

double PaperAccount::peakEquity() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_peakEquity;
}

double PaperAccount::dailyStartEquity() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dailyStartEquity;
}

void PaperAccount::resetDailyBaseline(double equity)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dailyStartEquity = equity;
}

std::string PaperAccount::accountId() const
{
    // Copy under the lock: restore() may reassign it.
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_accountId;
}

nlohmann::json PaperAccount::toJson() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const auto wallet_json = [](const Wallet &w) {
        return nlohmann::json{ { "cash_usdt", w.cash },
                               { "locked_margin_usdt", w.locked_margin },
                               { "realized_pnl_usdt", w.realized_pnl },
                               { "fees_paid_usdt", w.fees_paid } };
    };

    return nlohmann::json{
        { "account_id", m_accountId },
        { "spot", wallet_json(m_spot) },
        { "futures", wallet_json(m_futures) },
        { "cfd", wallet_json(m_cfd) },
        { "peak_equity_usdt", m_peakEquity },
        { "daily_start_equity_usdt", m_dailyStartEquity },
    };
}

bool PaperAccount::restore(const nlohmann::json &j)
{
    if (!j.is_object() || !j.contains("account_id")
        || !j.contains("spot") || !j.contains("futures")
        || !j.contains("cfd"))
    {
        return false;
    }

    const auto read_wallet = [](const nlohmann::json &w, Wallet &out) {
        if (!w.is_object() || !w.contains("cash_usdt")
            || !w.contains("locked_margin_usdt")
            || !w.contains("realized_pnl_usdt")
            || !w.contains("fees_paid_usdt"))
        {
            return false;
        }
        out.cash = w.at("cash_usdt").get<double>();
        out.locked_margin = w.at("locked_margin_usdt").get<double>();
        out.realized_pnl = w.at("realized_pnl_usdt").get<double>();
        out.fees_paid = w.at("fees_paid_usdt").get<double>();
        return true;
    };

    Wallet spot;
    Wallet futures;
    Wallet cfd;
    if (!read_wallet(j.at("spot"), spot) || !read_wallet(j.at("futures"), futures)
        || !read_wallet(j.at("cfd"), cfd))
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_accountId = j.at("account_id").get<std::string>();
    m_spot = spot;
    m_futures = futures;
    m_cfd = cfd;
    m_peakEquity = j.value("peak_equity_usdt", m_peakEquity);
    m_dailyStartEquity = j.value("daily_start_equity_usdt", m_dailyStartEquity);
    return true;
}

} // namespace pulse::execution
