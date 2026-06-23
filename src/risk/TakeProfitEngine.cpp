// take_profit_engine.cpp — Partial take-profit ladder (Layer 7 Risk Management)

#include "risk/TakeProfitEngine.hpp"

#include "logging/Logger.hpp"

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TakeProfitEngine::TakeProfitEngine(const TakeProfitConfig &default_config)
    : m_defaultConfig{ default_config }
{
}

// ---------------------------------------------------------------------------
// Position registration
// ---------------------------------------------------------------------------

void TakeProfitEngine::registerPosition(const std::string &position_id, const Position &pos,
    const TakeProfitConfig &config)
{
    // Register a position for take-profit monitoring:
    // 1. Acquire exclusive lock
    // 2. Use provided config or default
    // 3. Initialize TrackedTp with entry price, side, and next_target_idx = 0

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    TrackedTp tracked;
    // Use engine default when caller provides no targets (empty vector).
    tracked.config = config.targets_pct.empty() ? m_defaultConfig : config;
    tracked.entry_price = pos.entry_price;
    tracked.side = pos.side;
    tracked.next_target_idx = 0;

    m_tracked[position_id] = tracked;

    PULSE_LOG_INFO("risk",
        "Take-profit registered: {} entry={:.2f} targets={}",
        position_id, pos.entry_price, tracked.config.targets_pct.size());
}

void TakeProfitEngine::removePosition(const std::string &position_id)
{
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_tracked.erase(position_id);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

std::vector<TakeProfitEngine::TpSignal> TakeProfitEngine::evaluate(
    const std::unordered_map<std::string, Position> &current_positions)
{
    // Evaluate all tracked positions against their TP ladders:
    // 1. Acquire exclusive lock (advances next_target_idx)
    // 2. For each tracked position:
    //    a. Look up current position; skip if missing
    //    b. Skip if all targets already hit
    //    c. Compute target price from entry +/- targets_pct
    //    d. If price crossed target: emit signal, advance index

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    std::vector<TpSignal> signals;

    for (auto &[pos_id, tracked] : m_tracked)
    {
        // a. Look up current position.
        auto it = current_positions.find(pos_id);
        if (current_positions.end() == it)
        {
            continue;
        }

        const auto &pos = it->second;
        const Price current = pos.current_price;

        // b. Skip if all targets hit.
        if (tracked.next_target_idx >= static_cast<int>(tracked.config.targets_pct.size()))
        {
            continue;
        }

        // c. Compute target price.
        const double target_pct = tracked.config.targets_pct[tracked.next_target_idx];
        Price target_price;

        if (Side::Buy == tracked.side)
        {
            target_price = tracked.entry_price * (1.0 + target_pct);
        }
        else
        {
            target_price = tracked.entry_price * (1.0 - target_pct);
        }

        // d. Check if price crossed target.
        bool triggered = false;

        if (Side::Buy == tracked.side && current >= target_price)
        {
            triggered = true;
        }
        else if (Side::Sell == tracked.side && current <= target_price)
        {
            triggered = true;
        }

        if (triggered)
        {
            TpSignal signal;
            signal.position_id = pos_id;
            signal.close_fraction = (tracked.next_target_idx < static_cast<int>(tracked.config.fractions.size()))
                ? tracked.config.fractions[tracked.next_target_idx]
                : 1.0; // Default: close everything if fractions array is shorter.
            signal.target_index = tracked.next_target_idx;

            PULSE_LOG_INFO("risk",
                "Take-profit triggered: {} target #{} at {:.2f} (current: {:.2f}), close {:.2f}",
                pos_id, signal.target_index, target_price, current, signal.close_fraction);

            signals.push_back(signal);
            ++tracked.next_target_idx;
        }
    }

    return signals;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool TakeProfitEngine::isTracked(const std::string &position_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return m_tracked.count(position_id) > 0;
}

std::size_t TakeProfitEngine::trackedCount() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return m_tracked.size();
}

int TakeProfitEngine::nextTargetIndex(const std::string &position_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);

    auto it = m_tracked.find(position_id);
    if (m_tracked.end() == it)
    {
        return -1;
    }

    return it->second.next_target_idx;
}

} // namespace pulse::risk
