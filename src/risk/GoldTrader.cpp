// GoldTrader.cpp — see the header for the policy and threading contract

#include "risk/GoldTrader.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace pulse::risk
{

using namespace pulse::logging;

namespace
{

/// Whole seconds elapsed from `older` to `newer` (negative when newer < older).
std::int64_t secondsBetween(const Timestamp &newer, const Timestamp &older)
{
    return std::chrono::duration_cast<std::chrono::seconds>(newer - older).count();
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
GoldTrader::GoldTrader(const GoldTraderConfig &config,
    PositionManager &positions,
    PriceLookup price_lookup,
    SuperTrendLookup supertrend_lookup,
    CloseFn close,
    SetStopFn set_stop)
    : m_config{ config }, m_positions{ positions }, m_priceLookup{ std::move(price_lookup) },
      m_supertrendLookup{ std::move(supertrend_lookup) }, m_close{ std::move(close) }, m_setStop{ std::move(set_stop) }
{
}

// ---------------------------------------------------------------------------
// Stop helpers
// ---------------------------------------------------------------------------
bool GoldTrader::isTighter(Side side, Price candidate, Price reference)
{
    // Long stops sit below the market: the higher price triggers earlier.
    // Short stops sit above the market: the lower price triggers earlier.
    return (Side::Buy == side) ? (candidate > reference) : (candidate < reference);
}

Price GoldTrader::roundStop(Side side, Price price)
{
    // Round toward the SAFE side so price precision can never tighten a stop
    // by accident: floor a long stop (slightly lower), ceil a short stop
    // (slightly higher).
    const double scaled = price * kPriceScale;
    const double rounded = (Side::Buy == side) ? std::floor(scaled) : std::ceil(scaled);
    return rounded / kPriceScale;
}

bool GoldTrader::canRetry(const Timestamp &last, Timestamp now) const
{
    if (Timestamp{} == last)
    {
        return true;
    }

    // A backward wall-clock step must not push a pending retry further into
    // the future; retrying once more is safer than leaving the position
    // unprotected until the clock catches up.
    if (now <= last)
    {
        return true;
    }
    return secondsBetween(now, last) >= static_cast<std::int64_t>(m_config.retry_cooldown_sec);
}

std::pair<Price, const char *> GoldTrader::tightestCandidate(const Position &pos,
    double ounce_size,
    double peak_pnl,
    const std::optional<SuperTrendPoint> &supertrend,
    bool &trend_against) const
{
    trend_against = false;

    const double sign = (Side::Buy == pos.side) ? 1.0 : -1.0;
    const double usd_to_price = 1.0 / ounce_size;

    // Candidate 1: the hard loss stop. This is the only floor that applies
    // before the position has ever shown floating profit.
    Price best = pos.entry_price - sign * m_config.hard_stop_usd * usd_to_price;
    const char *reason = "hard_stop";

    // Candidate 2: break-even, armed once peak floating profit exceeded the
    // configured trigger. Peak (not current) profit keeps the stop armed
    // after a pullback — tighten-only semantics.
    if (peak_pnl > m_config.breakeven_trigger_usd)
    {
        if (isTighter(pos.side, pos.entry_price, best))
        {
            best = pos.entry_price;
            reason = "breakeven";
        }
    }

    // Candidate 3: the 1-minute SuperTrend line. The buffer is expressed in
    // USD and converted through the position's ounce size, so a 0.02-lot
    // position gets the same USD cushion as a 0.01-lot one. A trend that has
    // flipped against the position is an immediate exit, not a level.
    if (supertrend.has_value() && 0 != supertrend->direction)
    {
        const double buffer = m_config.supertrend_buffer_usd * usd_to_price;
        const bool agrees = (sign > 0.0 && supertrend->direction > 0) || (sign < 0.0 && supertrend->direction < 0);

        if (agrees)
        {
            const Price level = (sign > 0.0) ? (supertrend->value - buffer) : (supertrend->value + buffer);
            if (isTighter(pos.side, level, best))
            {
                best = level;
                reason = "supertrend";
            }
        }
        else
        {
            trend_against = true;
        }
    }

    // Candidate 4: peak give-back. Armed once the peak crossed
    // peak_trail_arm_usd (default 2 USD, the break-even trigger). Arming it
    // on ANY positive tick would make the -8 USD hard stop unreachable (a
    // +0.10 peak would put the stop at -4.90), which contradicts the explicit
    // hard-stop rule. Set the knob to 0 for the literal reading.
    if (peak_pnl > m_config.peak_trail_arm_usd)
    {
        const Price level = pos.entry_price + sign * (peak_pnl - m_config.peak_drawdown_usd) * usd_to_price;
        if (isTighter(pos.side, level, best))
        {
            best = level;
            reason = "peak_trail";
        }
    }

    // Candidate 5: the exchange-native stop already attached (possibly set
    // manually in the Gate app). It joins the comparison so the supervisor
    // can tighten it further but never loosens it.
    if (pos.sl_price > 0.0 && isTighter(pos.side, pos.sl_price, best))
    {
        best = pos.sl_price;
        reason = "exchange_sl";
    }

    return { best, reason };
}

// ---------------------------------------------------------------------------
// tick — one evaluation pass
// ---------------------------------------------------------------------------
GoldTrader::TickResult GoldTrader::tick(Timestamp now)
{
    TickResult result;

    if (!m_config.enabled)
    {
        return result;
    }

    // 1. Throttle to the configured cadence. The engine main loop runs at
    //    5 Hz; the policy only requires one sample per second.
    //
    //    A non-positive delta means the wall clock stepped backwards (NTP or
    //    resume-from-suspend RTC correction). Treating that as "too early"
    //    would suspend peak sampling and the stop ladder until the clock
    //    caught up, so a backward step is treated as due immediately.
    const auto since_last_poll = now - m_lastPoll;
    if (Timestamp{} != m_lastPoll && since_last_poll > std::chrono::nanoseconds::zero() &&
        since_last_poll < std::chrono::milliseconds(m_config.poll_interval_ms))
    {
        return result;
    }
    m_lastPoll = now;

    // 2. Snapshot the managed positions. Filtering by market type keeps a
    //    same-named instrument on another venue out of the supervisor.
    std::unordered_map<std::string, Position> live;

    for (const auto &pos : m_positions.getPositionsBySymbol(m_config.symbol))
    {
        if (m_config.market_type != pos.market_type)
        {
            continue;
        }
        live.emplace(pos.position_id, pos);
    }

    // 3. Drop per-position state for positions that no longer exist. This is
    //    what resets the peak after a close (manual or automatic).
    for (auto it = m_tracked.begin(); it != m_tracked.end();)
    {
        if (0 == live.count(it->first))
        {
            it = m_tracked.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (live.empty())
    {
        return result;
    }

    // 4. One live mark and one SuperTrend point drive every position on this
    //    symbol. A missing quote skips the pass: fabricating a mark could
    //    fire a stop the market never reached.
    const auto mark_opt = m_priceLookup ? m_priceLookup(m_config.symbol) : std::nullopt;
    if (!mark_opt.has_value() || *mark_opt <= 0.0)
    {
        PULSE_LOG_WARN("risk", "GoldTrader: no live {} quote - keeping current stops", m_config.symbol);
        return result;
    }
    const Price mark = *mark_opt;

    const auto supertrend = m_supertrendLookup ? m_supertrendLookup(m_config.symbol) : std::nullopt;

    // 5. Evaluate each position independently.
    for (const auto &[position_id, pos] : live)
    {
        auto &state = m_tracked[position_id];

        // 5a. Contract-volume sanity. XAUUSD is 100 oz per lot; when the CFD
        //     registry fails to load, quantoMultiplierFor() falls back to 1.0
        //     and the same USD thresholds would map to a 100x tighter price
        //     distance. Refuse to manage the position rather than silently
        //     stopping it out on noise.
        if (std::abs(pos.quanto_multiplier - m_config.contract_volume) > m_config.contract_volume * 0.01)
        {
            if (!state.quantoWarned)
            {
                state.quantoWarned = true;
                PULSE_LOG_ERROR("risk",
                    "GoldTrader: {} contract volume {} != expected {} - "
                    "refusing to manage (CFD symbol metadata missing?)",
                    position_id,
                    pos.quanto_multiplier,
                    m_config.contract_volume);
            }
            continue;
        }

        // 5b. Synced CFD positions reuse the engine-local id
        //     "<symbol>_<side>_sync". Closing and reopening the same side in
        //     the Gate app produces a DIFFERENT exchange position id behind
        //     the same engine id, so the state must reset or the dead
        //     position's peak and stop would follow the new one.
        if (!pos.exchange_position_id.empty())
        {
            if (!state.exchangePositionId.empty() && state.exchangePositionId != pos.exchange_position_id)
            {
                state = TrackedState{};
            }
            state.exchangePositionId = pos.exchange_position_id;
        }

        // Ounce-equivalent size: 0.01 lot XAUUSD * 100 oz/lot = 1 ounce.
        // The USD thresholds are converted to prices through this number.
        const double ounce_size = pos.quantity * pos.quanto_multiplier;
        if (ounce_size <= 0.0)
        {
            continue;
        }

        const double sign = (Side::Buy == pos.side) ? 1.0 : -1.0;
        const double pnl = (mark - pos.entry_price) * ounce_size * sign;

        // 5c. Track the peak as a PRICE, not as USD. The exchange re-averages
        //     entry_price when the user adds to the position, so a USD peak
        //     frozen at the old basis would fabricate a give-back level above
        //     the market (or understate it after averaging down). Deriving the
        //     USD peak from peakPrice + the current entry and size stays
        //     correct across adds, partial closes and re-averaging.
        if (!state.initialized)
        {
            state.initialized = true;
            state.peakPrice = mark;
            PULSE_LOG_INFO("risk",
                "GoldTrader: managing {} {} {:.3f} lots @ {:.2f} "
                "(mark {:.2f}, pnl {:.2f} USD, exchange stop {:.2f})",
                position_id,
                (Side::Buy == pos.side) ? "long" : "short",
                pos.quantity,
                pos.entry_price,
                mark,
                pnl,
                pos.sl_price);
        }
        else if (Side::Buy == pos.side)
        {
            state.peakPrice = std::max(state.peakPrice, mark);
        }
        else
        {
            state.peakPrice = std::min(state.peakPrice, mark);
        }

        const double peak_pnl = std::max(0.0, (state.peakPrice - pos.entry_price) * ounce_size * sign);

        // 5d. Reconcile the exchange-stop baseline with what the exchange
        //     actually holds. Adopting the CURRENT value (including 0 = no
        //     stop) lets the next push restore protection if the stop was
        //     removed or loosened in the app. A push still has to be tighter
        //     than this baseline, so the supervisor never loosens a stop.
        state.pushedStop = pos.sl_price;

        ++result.evaluated;

        // 6. A close already fired but the position is still visible (failed
        //    request or a slow exchange update). Wait out the cooldown before
        //    trying again — never spam close requests.
        if (state.closing && !canRetry(state.lastCloseAttempt, now))
        {
            continue;
        }
        state.closing = false;

        // 7. Merge the candidates and enforce tighten-only against the
        //    previously enforced stop.
        bool trend_against = false;
        auto [candidate, reason] = tightestCandidate(pos, ounce_size, peak_pnl, supertrend, trend_against);

        Price effective = candidate;
        if (state.effectiveStop > 0.0 && isTighter(pos.side, state.effectiveStop, effective))
        {
            effective = state.effectiveStop;
            reason = "previous";
        }

        if (effective != state.effectiveStop)
        {
            state.effectiveStop = effective;
            ++result.tightened;
            PULSE_LOG_INFO("risk",
                "GoldTrader: {} stop -> {:.2f} ({}, peak {:.2f} USD, pnl {:.2f})",
                position_id,
                effective,
                reason,
                peak_pnl,
                pnl);
        }

        // 8. Breach check. A SuperTrend flip against the position always
        //    exits, even if the level geometry has not reached the stop yet.
        const bool breached = trend_against || ((Side::Buy == pos.side) ? (mark <= effective) : (mark >= effective));

        if (breached)
        {
            state.lastCloseAttempt = now;
            state.closing = true;

            if (m_close && m_close(pos, mark))
            {
                ++result.closed;
                PULSE_LOG_WARN("risk",
                    "GoldTrader: CLOSE {} at {:.2f} ({}, pnl {:.2f} USD, "
                    "peak {:.2f}, stop {:.2f})",
                    position_id,
                    mark,
                    trend_against ? "supertrend_flip" : reason,
                    pnl,
                    peak_pnl,
                    effective);
                // The manager erases the position on a successful close; the
                // state entry disappears on the next tick.
                state.closing = true;
            }
            else
            {
                ++result.failed;
                PULSE_LOG_ERROR(
                    "risk", "GoldTrader: close failed for {} - retry in {}s", position_id, m_config.retry_cooldown_sec);
            }
            continue;
        }

        // 9. Mirror the tightened stop to the exchange-native price_sl. A
        //    stop on the wrong side of the market is never sent (the exchange
        //    would reject it); the breach branch above owns that case.
        if (!m_config.exchange_stop || !m_setStop)
        {
            continue;
        }

        const Price desired = roundStop(pos.side, effective);
        const bool needsPush = (0.0 == state.pushedStop) || isTighter(pos.side, desired, state.pushedStop);
        if (!needsPush)
        {
            continue;
        }

        const bool wrong_side = (Side::Buy == pos.side) ? (desired >= mark) : (desired <= mark);
        if (wrong_side || !canRetry(state.lastStopAttempt, now))
        {
            continue;
        }

        if (m_setStop(pos, desired))
        {
            state.pushedStop = desired;
            // The cooldown covers FAILED pushes only: a successful push must
            // not delay the next tightening (the exchange stop would trail
            // the policy by up to retry_cooldown_sec during a fast move).
            state.lastStopAttempt = Timestamp{};
            ++result.stopPushed;
            PULSE_LOG_INFO(
                "risk", "GoldTrader: {} exchange SL -> {:.2f} (tracked stop {:.2f})", position_id, desired, effective);
        }
        else
        {
            state.lastStopAttempt = now;
            PULSE_LOG_WARN("risk",
                "GoldTrader: exchange SL update failed for {} ({:.2f}) - "
                "retry in {}s",
                position_id,
                desired,
                m_config.retry_cooldown_sec);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
std::optional<Price> GoldTrader::effectiveStop(const std::string &position_id) const
{
    const auto it = m_tracked.find(position_id);
    if (m_tracked.end() == it || !it->second.initialized)
    {
        return std::nullopt;
    }
    return it->second.effectiveStop;
}

std::size_t GoldTrader::trackedPositions() const
{
    return m_tracked.size();
}

} // namespace pulse::risk
