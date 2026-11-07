#pragma once
// funding_watch.hpp — funding-rate window monitor (2026-09-06)
//
// Watches USDT-M perpetual funding rates (Gate public endpoint, 8h cadence,
// ~180-day retention) and lights up a signal-board window when the
// "crowd-long premium tax" is high AND persistent — the harvest condition of
// a spot-long / perp-short funding carry (7-year full-history validation
// 2026-09-06: with a 0.5bp gate, bull years 2020-21 paid +16~36%/yr net on
// 100U notional while flat years stay ≈0; see data/funding/).
//
// Semantics (deliberately boring):
//   - window OPEN   = all of the last `consec_events` funding applies
//                     (default 6 ≈ 48h) > `threshold` (default 0.0005 = 5bp).
//   - window CLOSED = any apply ≤ threshold. No hysteresis — funding applies
//                     only every 8h, so a window cannot flicker intraday.
//   - every poll publishes the current state to the signal board under
//     strategy_id "funding_watch_<SYMBOL>"; type Sell = window open ("short
//     side harvests funding"), Flat = closed. Transitions additionally log
//     a WARN so the operator sees the window open in the engine output.
//
// Pure observation: publishes to the board + logs only. It never places
// orders and never feeds the aggregator (board->publish is direct, so the
// auto_trade gate and the order path are bypassed entirely).
//
// Threading: NO own thread — the main loop calls tick() every ~200ms; a
// slow gate inside keeps REST polls to `poll_sec` (default 30 min, plenty:
// funding moves 3×/day). REST calls hold the shared rest_mutex. All
// decision logic is in pure free functions (fundingWindowOpen /
// fundingRatesFromJson) — unit-tested without any network.

#include "core/config.hpp"
#include "exchange/GateRestClient.hpp"
#include "strategy/signal/SignalBoard.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::funding
{

/// Window condition (pure): `newest_first` holds funding rates in DESCENDING
/// time order (index 0 = most recent apply). True iff there are at least
/// `consec_events` entries and every one of the most recent `consec_events`
/// is strictly greater than `threshold`.
[[nodiscard]] bool fundingWindowOpen(const std::vector<double> &newest_first,
                                     double threshold, int consec_events);

/// Parse a Gate funding_rate response body (JSON array of {"r": string,
/// "t": sec}, newest first) into rates, keeping at most `max` entries.
/// Non-array bodies yield an empty vector (caller treats as transient).
[[nodiscard]] std::vector<double> fundingRatesFromJson(
    const nlohmann::json &body, std::size_t max);

/// Strategy-id prefix for board rows: funding_watch_<SYMBOL>.
constexpr const char *kBoardPrefix = "funding_watch_";

class FundingWatch
{
  public:
    FundingWatch(const FundingWatchConfig &cfg, strategy::SignalBoard &board,
                 exchange::GateRestClient &futures_rest,
                 std::mutex &rest_mutex);

    /// Main-loop slot (~200ms). Polls at most once per poll_sec; no-op when
    /// disabled. Never blocks long: one small REST GET per symbol per poll.
    void tick(std::int64_t now_ms);

  private:
    void pollSymbol(const std::string &symbol, std::int64_t now_ms);
    void publish(const std::string &symbol, bool window_open, bool announce,
                 const std::vector<double> &rates);

    const FundingWatchConfig &m_cfg;
    strategy::SignalBoard &m_board;
    exchange::GateRestClient &m_rest;
    std::mutex &m_rest_mutex;

    std::int64_t m_next_poll_ms{ 0 };
    std::int64_t m_last_err_log_ms{ 0 };
    /// Last published window state per symbol (WARN only on transitions).
    std::unordered_map<std::string, bool> m_last_open;
};

} // namespace pulse::funding
