#pragma once
// GoldTrader.hpp — USD-based dynamic stop supervisor for XAUUSD CFD
//
// Layer 7 (Risk Management). The "gold trader" is not an entry strategy: it
// supervises every open XAUUSD CFD position (including positions opened
// manually in the Gate app and imported by sync_positions) and enforces the
// strategy-1 dynamic stop ladder:
//
//   1. Hard stop     — unrealized loss reaches hard_stop_usd (default 8 USD).
//   2. Break-even    — once peak floating profit exceeds
//                      breakeven_trigger_usd (default 2 USD), the stop is
//                      raised to the entry price.
//   3. SuperTrend    — the 1-minute SuperTrend line participates as a stop
//                      candidate, placed supertrend_buffer_usd beyond the line
//                      (default 1 USD) so a wick does not stop the trade out.
//                      A SuperTrend flip against the position closes at market.
//   4. Peak give-back — peak floating profit is sampled every
//                      poll_interval_ms (default 1 s); the level
//                      peak_pnl - peak_drawdown_usd (default 5 USD) also
//                      participates. Trading through it closes at market
//                      immediately.
//   5. Tighten-only   — the effective stop is monotonic: candidates are
//                      merged by taking the tighter one, and a previously
//                      tighter stop is never loosened (including a manually
//                      tightened exchange stop).
//
// Every tightened stop is mirrored to the exchange-native price_sl (via the
// injected SetStop callback, wired to EngineServices::modify_sl_tp), so the
// position keeps a protective stop if the engine process dies. The peak
// give-back rule stays engine-side because it needs a per-second mark.
//
// Prices are read from the live ticker cache, never from
// Position::current_price (only refreshed by the ~10 s position sync).
// Unrealized PnL is recomputed locally with the position's true ounce size:
//   pnl = (mark - entry) * quantity * quanto_multiplier * side_sign
// For XAUUSD 0.01 lot (1 ounce) one USD of PnL equals a 1.00 price move.
//
// Thread safety:
//   - tick() is single-threaded: call it only from the engine main loop.
//   - PositionManager is internally synchronized; every other callback is
//     invoked from the main loop, exactly like ProtectiveExitManager.
//
// Failure handling:
//   - A missing ticker keeps the previous state and fabricates no stop.
//   - Failed closes and failed stop pushes are retried after
//     retry_cooldown_sec instead of spamming the exchange every tick.
//   - Stop levels are rounded toward the safe side (floor for longs, ceil for
//     shorts) so price precision never tightens a stop unintentionally.

#include "core/config.hpp"
#include "core/types.hpp"
#include "risk/PositionManager.hpp"
#include "risk/SuperTrend.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace pulse::risk
{

// ---------------------------------------------------------------------------
// GoldTrader — dynamic stop supervisor for one symbol / market
// ---------------------------------------------------------------------------
class GoldTrader
{
  public:
    /// Latest trade price for the managed symbol. Return nullopt when the
    /// ticker cache has no usable quote — the pass is skipped, no stop fires.
    using PriceLookup = std::function<std::optional<Price>(const Symbol &symbol)>;

    /// Latest 1-minute SuperTrend point for the managed symbol. Return
    /// nullopt while the kline buffer is warming up.
    using SuperTrendLookup = std::function<std::optional<SuperTrendPoint>(const Symbol &symbol)>;

    /// Close the whole position at market. `mark` is the price used for the
    /// decision (the caller may refresh the risk view with it before closing).
    /// Returns true when the close request was accepted.
    using CloseFn = std::function<bool(const Position &pos, Price mark)>;

    /// Mirror a tightened stop to the exchange-native price_sl. Returns true
    /// on success; a failed push is retried after retry_cooldown_sec.
    using SetStopFn = std::function<bool(const Position &pos, Price sl_price)>;

    GoldTrader(const GoldTraderConfig &config,
        PositionManager &positions,
        PriceLookup price_lookup,
        SuperTrendLookup supertrend_lookup,
        CloseFn close,
        SetStopFn set_stop);

    /// Outcome counters for one tick (logging + tests).
    struct TickResult
    {
        int evaluated{ 0 };  ///< Positions priced and evaluated this pass.
        int tightened{ 0 };  ///< Effective stops that moved tighter.
        int closed{ 0 };     ///< Positions closed at market this pass.
        int failed{ 0 };     ///< Failed close requests (retry scheduled).
        int stopPushed{ 0 }; ///< Exchange-side stops successfully updated.
    };

    /// Run one evaluation pass. Call from the engine main loop; the internal
    /// poll_interval_ms throttle makes the extra calls free.
    TickResult tick(Timestamp now);

    /// Effective (tightest) stop currently enforced for a position, or
    /// nullopt when the position is not tracked. Exposed for tests and logs.
    [[nodiscard]] std::optional<Price> effectiveStop(const std::string &position_id) const;

    /// Number of positions currently tracked.
    [[nodiscard]] std::size_t trackedPositions() const;

    /// Snapshot for the control plane / Qt status card.
    ///
    /// Shape:
    ///   armed, symbol, market_type, config{}, tracked_positions[] with
    ///   position_id, exchange_position_id, side, quantity, entry_price,
    ///   mark_price, unrealized_pnl, peak_pnl, effective_stop, exchange_stop.
    ///
    /// Thread-safe: the JSON is rebuilt at the end of every tick and copied
    /// under a small mutex, so a control-plane caller never waits on a REST
    /// call held by the main loop.
    [[nodiscard]] nlohmann::json status() const;

    /// JSON view of the static configuration (also used when the supervisor
    /// is disabled, so the UI can show the thresholds it would use).
    [[nodiscard]] static nlohmann::json configJson(const GoldTraderConfig &config);

    /// Exchange price precision for XAUUSD (two decimals).
    static constexpr double kPriceScale = 100.0;

  private:
    /// Per-position state. The peak is stored as a PRICE (the best mark seen),
    /// not as USD: the exchange can re-average entry_price when the user adds
    /// to the position, and a USD peak frozen at the old basis would fabricate
    /// a give-back level above the market. The USD peak is derived on every
    /// pass from peakPrice, the current entry and the current ounce size.
    struct TrackedState
    {
        bool initialized{ false };      ///< First sample seen (peak seeded).
        double peakPrice{ 0.0 };        ///< Best mark seen (high long / low short).
        double effectiveStop{ 0.0 };    ///< Tightest stop enforced so far.
        double pushedStop{ 0.0 };       ///< Last stop accepted by the exchange.
        std::string exchangePositionId; ///< Detects sync-id reuse after a reopen.
        Timestamp lastCloseAttempt{};   ///< Last market-close attempt.
        Timestamp lastStopAttempt{};    ///< Last FAILED stop-push attempt.
        bool closing{ false };          ///< A close was fired; wait for it.
        bool quantoWarned{ false };     ///< Mismatched contract volume logged.

        // Last evaluated snapshot (display only; never drives a decision).
        Side side{ Side::Buy };
        double quantity{ 0.0 };
        double entryPrice{ 0.0 };
        double lastMark{ 0.0 };
        double lastPnl{ 0.0 };
        double lastPeakPnl{ 0.0 };
    };

    /// Compute the tightest candidate stop for one position.
    ///
    /// Candidates:
    ///   1. hard stop at entry -/+ hard_stop_usd / ounce_size
    ///   2. break-even at entry once peak PnL exceeds the trigger
    ///   3. SuperTrend line +/- buffer (only when the trend agrees)
    ///   4. peak PnL - peak_drawdown_usd (armed once peak PnL exceeds
    ///      peak_trail_arm_usd)
    ///   5. an existing exchange stop (never loosen the user's own stop)
    ///
    /// Returns the candidate price and a short reason string. `trend_against`
    /// is set when the SuperTrend direction opposes the position; that is an
    /// immediate exit regardless of the level geometry.
    [[nodiscard]] std::pair<Price, const char *> tightestCandidate(const Position &pos,
        double ounce_size,
        double peak_pnl,
        const std::optional<SuperTrendPoint> &supertrend,
        bool &trend_against) const;

    /// True when `candidate` is tighter than `reference` for the given side.
    [[nodiscard]] static bool isTighter(Side side, Price candidate, Price reference);

    /// Round a stop price toward the safe side (floor long / ceil short).
    [[nodiscard]] static Price roundStop(Side side, Price price);

    /// True when `last` is empty or at least retry_cooldown_sec has elapsed.
    [[nodiscard]] bool canRetry(const Timestamp &last, Timestamp now) const;

    GoldTraderConfig m_config;
    PositionManager &m_positions;
    PriceLookup m_priceLookup;
    SuperTrendLookup m_supertrendLookup;
    CloseFn m_close;
    SetStopFn m_setStop;

    /// Rebuild the control-plane snapshot from the current per-position state.
    /// Called only from the main loop (end of tick / constructor).
    void publishStatus();

    std::unordered_map<std::string, TrackedState> m_tracked;
    Timestamp m_lastPoll{};

    /// Control-plane snapshot. The mutex is deliberately NOT held during
    /// tick()'s REST work: status() only ever copies the last published JSON.
    mutable std::mutex m_statusMutex;
    nlohmann::json m_statusSnapshot;
};

} // namespace pulse::risk
