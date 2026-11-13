// ProtectiveExitManager.cpp — see the header for the evaluation contract

#include "risk/ProtectiveExitManager.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <chrono>
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
ProtectiveExitManager::ProtectiveExitManager(const RiskConfig &config,
                                             PositionManager &positions,
                                             PriceLookup price_lookup,
                                             CloseFn close,
                                             ManageFilter should_manage)
    : m_config{ config }
    , m_positions{ positions }
    , m_priceLookup{ std::move(price_lookup) }
    , m_close{ std::move(close) }
    , m_shouldManage{ std::move(should_manage) }
    , m_stops{ config.stop_loss }
    , m_takeProfits{ config.take_profit }
{
}

// ---------------------------------------------------------------------------
// Cooldown bookkeeping
// ---------------------------------------------------------------------------
bool ProtectiveExitManager::canFire(const std::string &position_id, Timestamp now) const
{
    const auto it = m_lastAttempt.find(position_id);
    if (m_lastAttempt.end() == it)
    {
        return true;
    }
    return secondsBetween(now, it->second)
           >= static_cast<std::int64_t>(kRetryCooldownSeconds);
}

void ProtectiveExitManager::markFired(const std::string &position_id, Timestamp now)
{
    m_lastAttempt[position_id] = now;
}

// ---------------------------------------------------------------------------
// tick — one evaluation pass
// ---------------------------------------------------------------------------
ProtectiveExitManager::TickResult ProtectiveExitManager::tick(Timestamp now)
{
    TickResult result;

    // 1. Snapshot every open position and refresh its mark price.
    //
    //    A missing quote keeps the position's previous mark: fabricating a
    //    price from a stale feed could trigger a stop that the market never
    //    reached.
    auto positions = m_positions.getAllPositions();
    std::unordered_map<std::string, Position> by_id;
    by_id.reserve(positions.size());

    for (auto &pos : positions)
    {
        // Ownership filter: a position managed by a more specific supervisor
        // (the gold trader) must not also be acted on by the generic
        // percentage ladder. Excluded positions are absent from by_id below,
        // so any previous registration is dropped in step 2b.
        if (m_shouldManage && !m_shouldManage(pos))
        {
            continue;
        }

        if (m_priceLookup)
        {
            auto price = m_priceLookup(pos.symbol, pos.market_type);
            if (price.has_value() && *price > 0.0)
            {
                pos.current_price = *price;
            }
        }
        by_id.emplace(pos.position_id, pos);
    }

    // 2a. Register positions that appeared since the last tick.
    for (const auto &[id, pos] : by_id)
    {
        if (0 != m_registered.count(id))
        {
            continue;
        }
        m_stops.registerPosition(id, pos, m_config.stop_loss);
        if (m_config.take_profit.enabled)
        {
            m_takeProfits.registerPosition(id, pos, m_config.take_profit);
        }
        m_registered.insert(id);
    }

    // 2b. Drop positions that are no longer open (closed manually, by the
    //     exchange, or by a previous protective exit).
    for (auto it = m_registered.begin(); it != m_registered.end();)
    {
        if (0 == by_id.count(*it))
        {
            m_stops.removePosition(*it);
            m_takeProfits.removePosition(*it);
            m_lastAttempt.erase(*it);
            it = m_registered.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // 3. Stop-loss ladder — always a FULL close of the remaining quantity.
    for (const auto &position_id : m_stops.evaluate(by_id, now))
    {
        const auto pos_it = by_id.find(position_id);
        if (by_id.end() == pos_it || !canFire(position_id, now))
        {
            continue;
        }

        markFired(position_id, now);
        if (m_close && m_close(pos_it->second, pos_it->second.quantity))
        {
            ++result.stopped;
            PULSE_LOG_WARN("risk", "Stop-loss close placed for {} {} ({})",
                           position_id, pos_it->second.symbol,
                           toString(pos_it->second.market_type));
        }
        else
        {
            ++result.failed;
            PULSE_LOG_ERROR("risk", "Stop-loss close failed for {} — retry in {}s",
                            position_id, kRetryCooldownSeconds);
        }
    }

    // 4. Take-profit ladder — partial close of the remaining quantity.
    //
    //    NOTE: TakeProfitEngine advances the target index when it emits the
    //    signal, so a failed close loses that target rather than replaying it.
    //    Failing loudly (failed counter + log) is preferred over silently
    //    closing the same target twice.
    if (m_config.take_profit.enabled)
    {
        for (const auto &signal : m_takeProfits.evaluate(by_id))
        {
            const auto pos_it = by_id.find(signal.position_id);
            if (by_id.end() == pos_it || !canFire(signal.position_id, now))
            {
                continue;
            }

            const double fraction = std::clamp(signal.close_fraction, 0.0, 1.0);
            const double quantity = pos_it->second.quantity * fraction;
            if (quantity <= 0.0)
            {
                continue;
            }

            markFired(signal.position_id, now);
            if (m_close && m_close(pos_it->second, quantity))
            {
                ++result.took_profit;
                PULSE_LOG_INFO("risk",
                               "Take-profit close placed for {} {} (target #{}, {:.1f}%)",
                               signal.position_id, pos_it->second.symbol,
                               signal.target_index, fraction * 100.0);
            }
            else
            {
                ++result.failed;
                PULSE_LOG_ERROR("risk", "Take-profit close failed for {} — retry in {}s",
                                signal.position_id, kRetryCooldownSeconds);
            }
        }
    }

    return result;
}

std::size_t ProtectiveExitManager::trackedPositions() const
{
    return m_registered.size();
}

} // namespace pulse::risk