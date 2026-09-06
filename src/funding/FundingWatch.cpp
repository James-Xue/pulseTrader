// funding_watch.cpp — funding-rate window monitor (see FundingWatch.hpp)

#include "funding/FundingWatch.hpp"

#include "core/PulseError.hpp"
#include "core/types.hpp"
#include "logging/Logger.hpp"
#include "strategy/signal_types.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>

namespace pulse::funding
{

bool fundingWindowOpen(const std::vector<double> &newest_first,
                       double threshold, int consec_events)
{
    if (consec_events < 1
        || static_cast<std::size_t>(consec_events) > newest_first.size())
    {
        return false;
    }
    // Strictly greater: a rate exactly at the threshold is NOT a window —
    // the gate exists to separate real premium regimes from noise.
    return std::all_of(newest_first.begin(),
                       newest_first.begin() + consec_events,
                       [threshold](double r) { return r > threshold; });
}

std::vector<double> fundingRatesFromJson(const nlohmann::json &body,
                                         std::size_t max)
{
    std::vector<double> rates;
    if (!body.is_array())
    {
        return rates;
    }
    rates.reserve(std::min(max, body.size()));
    for (const auto &rec : body)
    {
        if (!rec.is_object() || !rec.contains("r") || !rec["r"].is_string())
        {
            continue; // Malformed row — skip, do not fail the poll.
        }
        try
        {
            rates.push_back(std::stod(rec["r"].get<std::string>()));
        }
        catch (const std::exception &)
        {
            continue;
        }
        if (rates.size() >= max)
        {
            break;
        }
    }
    return rates; // Server order is newest-first (verified 2026-09-06).
}

// ---------------------------------------------------------------------------
// FundingWatch
// ---------------------------------------------------------------------------

FundingWatch::FundingWatch(const FundingWatchConfig &cfg,
                           strategy::SignalBoard &board,
                           exchange::GateRestClient &futures_rest,
                           std::mutex &rest_mutex)
    : m_cfg{ cfg }
    , m_board{ board }
    , m_rest{ futures_rest }
    , m_rest_mutex{ rest_mutex }
{
}

void FundingWatch::tick(std::int64_t now_ms)
{
    if (!m_cfg.enabled || m_cfg.symbols.empty())
    {
        return;
    }
    if (now_ms < m_next_poll_ms)
    {
        return;
    }
    // Funding applies every 8h — poll well below that, but never busy-poll.
    m_next_poll_ms = now_ms + static_cast<std::int64_t>(m_cfg.poll_sec) * 1000;
    for (const auto &symbol : m_cfg.symbols)
    {
        pollSymbol(symbol, now_ms);
    }
}

void FundingWatch::pollSymbol(const std::string &symbol, std::int64_t now_ms)
{
    // Funding rates live on the shared futures REST client; keep the global
    // REST serialization discipline (never hold m_rest_mutex elsewhere).
    std::lock_guard lock{ m_rest_mutex };
    const int limit = std::max(m_cfg.consec_events + 6, 12);
    auto res = m_rest.getFuturesFundingRate(symbol, limit);
    if (!ok(res))
    {
        if (now_ms - m_last_err_log_ms > 300'000)
        {
            m_last_err_log_ms = now_ms;
            PULSE_LOG_WARN("funding_watch", "{}: poll failed: {}", symbol,
                           error(res).message);
        }
        return;
    }

    const auto rates = fundingRatesFromJson(value(res),
                                            static_cast<std::size_t>(limit));
    if (rates.empty())
    {
        if (now_ms - m_last_err_log_ms > 300'000)
        {
            m_last_err_log_ms = now_ms;
            PULSE_LOG_WARN("funding_watch", "{}: empty funding response "
                           "(transient?)", symbol);
        }
        return;
    }

    const bool open = fundingWindowOpen(rates, m_cfg.threshold,
                                        m_cfg.consec_events);
    const bool was_open = m_last_open[symbol]; // operator[] → default false
    m_last_open[symbol] = open;
    publish(symbol, open, open != was_open, rates);
}

void FundingWatch::publish(const std::string &symbol, bool window_open,
                           bool announce, const std::vector<double> &rates)
{
    strategy::TradingSignal sig;
    sig.type = window_open ? strategy::SignalType::Sell
                           : strategy::SignalType::Flat;
    sig.symbol = symbol;
    sig.market_type = MarketType::Futures;
    sig.confidence = window_open ? 1.0 : 0.0;
    sig.price = 0.0; // Observation row only — no market price is fetched.
    sig.strategy_id = std::string{ kBoardPrefix } + symbol;
    sig.timestamp = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now());

    const double latest = rates.front();
    const double avg24h = std::accumulate(
        rates.begin(), rates.begin() + std::min<std::size_t>(rates.size(), 3),
        0.0) / static_cast<double>(std::min<std::size_t>(rates.size(), 3));
    sig.indicators = {
        { "funding_latest", latest },
        { "funding_avg_24h", avg24h },
        { "window_threshold", m_cfg.threshold },
        { "window_events", m_cfg.consec_events },
        { "window_8h_events", static_cast<int>(rates.size()) },
    };
    sig.reason = window_open
        ? "funding window OPEN: 做空收租窗口(连续 "
            + std::to_string(m_cfg.consec_events) + " 次结算 > "
            + std::to_string(m_cfg.threshold) + ")"
        : "funding window CLOSED: 费率回落至闸下(latest="
            + std::to_string(latest) + ")";

    m_board.publish(sig);

    // Announce transitions only (a window lasts days once open; per-poll
    // logging would spam 48 lines/day). Board row refreshes every poll.
    if (announce)
    {
        PULSE_LOG_WARN("funding_watch", "{} funding window {} "
                       "(latest={:.6f} avg24h={:.6f}, threshold={:.6f})",
                       symbol, window_open ? "OPEN — 做空收租窗口"
                                           : "CLOSED",
                       latest, avg24h, m_cfg.threshold);
    }
}

} // namespace pulse::funding
