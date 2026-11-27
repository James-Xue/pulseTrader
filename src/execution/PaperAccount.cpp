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

const char *legToString(PaperLeg leg) noexcept
{
    switch (leg)
    {
    case PaperLeg::Futures:
        return "futures";
    case PaperLeg::Cfd:
        return "cfd";
    default:
        return "spot";
    }
}

PaperAccount::PaperAccount(const PaperConfig &cfg)
    : m_accountId{ cfg.account_id }
    , m_initialBalance{ cfg.initial_balance_usdt }
{
    // Every leg starts with the same seeded balance, mirroring Gate's
    // independent wallets rather than splitting one total (see the design
    // note in docs/paper-trading.md).
    m_spot.cash = cfg.initial_balance_usdt;
    m_futures.cash = cfg.initial_balance_usdt;
    m_cfd.cash = cfg.initial_balance_usdt;
    m_spot.deposits = cfg.initial_balance_usdt;
    m_futures.deposits = cfg.initial_balance_usdt;
    m_cfd.deposits = cfg.initial_balance_usdt;

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
PaperWalletSnapshot snapshotOf(const auto &w, double unrealized)
{
    PaperWalletSnapshot s;
    s.cash_usdt = w.cash;
    s.locked_margin_usdt = w.locked_margin;
    s.realized_pnl_usdt = w.realized_pnl;
    s.fees_paid_usdt = w.fees_paid;
    s.unrealized_pnl_usdt = unrealized;
    s.margin_free_usdt = w.cash;
    return s;
}
} // namespace

PaperAccountSnapshot PaperAccount::snapshot(double spot_unrealized,
                                            double futures_unrealized,
                                            double cfd_unrealized) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    PaperAccountSnapshot s;
    s.account_id = m_accountId;
    s.unrealized_pnl_usdt =
        spot_unrealized + futures_unrealized + cfd_unrealized;
    s.spot = snapshotOf(m_spot, spot_unrealized);
    s.futures = snapshotOf(m_futures, futures_unrealized);
    s.cfd = snapshotOf(m_cfd, cfd_unrealized);
    s.equity_usdt = m_spot.cash + m_spot.locked_margin + m_futures.cash
        + m_futures.locked_margin + m_cfd.cash + m_cfd.locked_margin
        + s.unrealized_pnl_usdt;
    s.peak_equity_usdt = m_peakEquity;
    s.daily_start_equity_usdt = m_dailyStartEquity;
    return s;
}

void PaperAccount::releaseOrphanedMargin()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Wallet *w : { &m_spot, &m_futures, &m_cfd })
    {
        w->cash += w->locked_margin;
        w->locked_margin = 0.0;
    }
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

CapitalEvent PaperAccount::applyCapital(PaperLeg leg, const std::string &kind,
                                        double amount_usdt,
                                        const std::string &note,
                                        std::int64_t ts_ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Wallet &w = wallet(leg);

    const bool known = ("seed" == kind || "reset" == kind || "topup" == kind
                        || "withdraw" == kind);
    if (!known)
    {
        // Unknown kinds never move money; the control plane validates first.
        return CapitalEvent{};
    }
    if ("reset" == kind && 0.0 < w.locked_margin)
    {
        // A reset must not erase locked margin: the position behind it would
        // lose its collateral (audit P4). The caller must flatten first.
        return CapitalEvent{};
    }

    if ("seed" == kind || "reset" == kind)
    {
        w.cash = amount_usdt;
        w.deposits = amount_usdt;
        if ("reset" == kind)
        {
            // A reset opens a new capital epoch: the identity restarts at the
            // new deposit, so the old realized/fee counters are archived in
            // the event log rather than carried into the new epoch.
            w.realized_pnl = 0.0;
            w.fees_paid = 0.0;
        }
    }
    else
    {
        // topup/withdraw amounts are positive inputs; a withdraw subtracts.
        const double signed_amount =
            ("withdraw" == kind) ? -amount_usdt : amount_usdt;
        w.cash += signed_amount;
        w.deposits += signed_amount;
    }

    const double recorded_amount = ("withdraw" == kind) ? -amount_usdt
                                                        : amount_usdt;
    CapitalEvent event;
    event.id = "cap:" + std::string(legToString(leg)) + ":" + kind + ":"
        + std::to_string(ts_ms);
    event.ts_ms = ts_ms;
    event.leg = legToString(leg);
    event.kind = kind;
    event.amount_usdt = recorded_amount;
    event.cash_after = w.cash;
    event.realized_after = w.realized_pnl;
    event.fees_after = w.fees_paid;
    event.note = note;
    m_capitalEvents.push_back(event);
    return event;
}

bool PaperAccount::recordAdjustmentOnce(const std::string &id, PaperLeg leg,
                                        const std::string &kind,
                                        double realized_delta, double fees_delta,
                                        const std::string &note,
                                        std::int64_t ts_ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (id.empty())
    {
        return false;
    }
    for (const auto &existing : m_capitalEvents)
    {
        if (existing.id == id)
        {
            return false;
        }
    }
    Wallet &w = wallet(leg);
    CapitalEvent event;
    event.id = id;
    event.ts_ms = ts_ms;
    event.leg = legToString(leg);
    event.kind = kind;
    event.realized_delta = realized_delta;
    event.fees_delta = fees_delta;
    event.cash_after = w.cash;
    event.realized_after = w.realized_pnl;
    event.fees_after = w.fees_paid;
    event.note = note;
    m_capitalEvents.push_back(event);
    return true;
}

std::vector<CapitalEvent> PaperAccount::capitalEvents() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_capitalEvents;
}

bool PaperAccount::hasCapitalEvent(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &event : m_capitalEvents)
    {
        if (event.id == id)
        {
            return true;
        }
    }
    return false;
}

double PaperAccount::netDeposits(PaperLeg leg) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return wallet(leg).deposits;
}

double PaperAccount::identityResidual(PaperLeg leg) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Wallet &w = wallet(leg);
    // cash + locked == deposits + realized - fees
    const double left = w.cash + w.locked_margin;
    const double right = w.deposits + w.realized_pnl - w.fees_paid;
    return left - right;
}

nlohmann::json PaperAccount::toJson() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    const auto wallet_json = [](const Wallet &w) {
        return nlohmann::json{ { "cash_usdt", w.cash },
                               { "locked_margin_usdt", w.locked_margin },
                               { "realized_pnl_usdt", w.realized_pnl },
                               { "fees_paid_usdt", w.fees_paid },
                               { "deposits_usdt", w.deposits } };
    };
    const auto event_json = [](const CapitalEvent &e) {
        return nlohmann::json{ { "id", e.id },
                               { "ts_ms", e.ts_ms },
                               { "leg", e.leg },
                               { "kind", e.kind },
                               { "amount_usdt", e.amount_usdt },
                               { "realized_delta", e.realized_delta },
                               { "fees_delta", e.fees_delta },
                               { "cash_after", e.cash_after },
                               { "realized_after", e.realized_after },
                               { "fees_after", e.fees_after },
                               { "note", e.note } };
    };

    return nlohmann::json{
        { "account_id", m_accountId },
        { "spot", wallet_json(m_spot) },
        { "futures", wallet_json(m_futures) },
        { "cfd", wallet_json(m_cfd) },
        { "peak_equity_usdt", m_peakEquity },
        { "daily_start_equity_usdt", m_dailyStartEquity },
        { "capital_events", [&] {
             nlohmann::json events = nlohmann::json::array();
             for (const auto &event : m_capitalEvents)
             {
                 events.push_back(event_json(event));
             }
             return events;
         }() },
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
        // v2 files predate deposits: default to the configured seed.
        out.deposits = w.value("deposits_usdt", 0.0);
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
    if (0.0 == m_spot.deposits && 0.0 == m_futures.deposits
        && 0.0 == m_cfd.deposits)
    {
        // Legacy v2 file without the counter: fall back to the configured
        // seed so the identity is meaningful instead of always failing.
        m_spot.deposits = m_initialBalance;
        m_futures.deposits = m_initialBalance;
        m_cfd.deposits = m_initialBalance;
    }
    m_capitalEvents.clear();
    if (j.contains("capital_events") && j["capital_events"].is_array())
    {
        for (const auto &item : j["capital_events"])
        {
            if (!item.is_object())
            {
                continue;
            }
            CapitalEvent event;
            event.id = item.value("id", std::string{});
            event.ts_ms = item.value("ts_ms", std::int64_t{ 0 });
            event.leg = item.value("leg", std::string{});
            event.kind = item.value("kind", std::string{});
            event.amount_usdt = item.value("amount_usdt", 0.0);
            event.realized_delta = item.value("realized_delta", 0.0);
            event.fees_delta = item.value("fees_delta", 0.0);
            event.cash_after = item.value("cash_after", 0.0);
            event.realized_after = item.value("realized_after", 0.0);
            event.fees_after = item.value("fees_after", 0.0);
            event.note = item.value("note", std::string{});
            m_capitalEvents.push_back(std::move(event));
        }
    }
    return true;
}

int seedLegacyReconciliationAdjustments(PaperAccount &account,
                                        std::int64_t ts_ms)
{
    // The four capital epochs the audit reconstructed. These move no cash:
    // they document, per epoch, how far the engine's trade ledger sat from the
    // account snapshot before the P1/P2 fixes. A = E0, B = E1, C = E2, D = E3
    // (the audit's §2 naming). Idempotent by id, so a restart (or a second
    // call) adds nothing.
    struct LegacyAdjustment
    {
        const char *id;
        double realized_delta;
        double fees_delta;
        const char *note;
    };
    static constexpr LegacyAdjustment kAdjustments[] = {
        { "legacy:E0", -5.91, 0.00, "E0 gap (audit 2026-11-27 finding #6)" },
        { "legacy:E1", -10.07, -0.54, "E1 gap (findings #1/#2)" },
        { "legacy:E2", 0.00, -0.06, "E2 fee gap (finding #2)" },
        { "legacy:E3", 0.00, 0.00, "E3 reconciles to the cent (no adjustment)" },
    };

    int added = 0;
    for (const auto &adjustment : kAdjustments)
    {
        if (account.recordAdjustmentOnce(adjustment.id, PaperLeg::Cfd,
                                         "legacy_adjustment",
                                         adjustment.realized_delta,
                                         adjustment.fees_delta,
                                         adjustment.note, ts_ms))
        {
            ++added;
        }
    }
    return added;
}

} // namespace pulse::execution
