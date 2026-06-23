// stop_loss_engine.cpp — Stop-loss evaluator (Layer 7 Risk Management)

#include "risk/stop_loss_engine.hpp"

#include "logging/logger.hpp"

#include <chrono>

namespace pulse::risk
{

using namespace pulse::logging;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

StopLossEngine::StopLossEngine(const StopLossConfig &default_config)
    : m_defaultConfig{ default_config }
{
}

// ---------------------------------------------------------------------------
// Position registration
// ---------------------------------------------------------------------------

void StopLossEngine::registerPosition(const std::string &position_id, const Position &pos,
    const StopLossConfig &config)
{
    // Register a position for stop-loss monitoring:
    // 1. Acquire exclusive lock
    // 2. Use provided config or default
    // 3. Initialize TrackedStop with entry price and open time
    // 4. For trailing stop, initialize best_price to entry price

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    TrackedStop tracked;
    tracked.config = (StopMode::Fixed == config.mode && 0.0 == config.fixed_pct)
        ? m_defaultConfig : config;
    tracked.entry_price = pos.entry_price;
    tracked.side = pos.side;
    tracked.open_time = pos.open_time;
    tracked.best_price = pos.entry_price; // Start tracking from entry.

    m_tracked[position_id] = tracked;

    PULSE_LOG_INFO("risk",
        "Stop-loss registered: {} mode={} entry={:.2f}",
        position_id, static_cast<int>(tracked.config.mode), pos.entry_price);
}

void StopLossEngine::removePosition(const std::string &position_id)
{
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_tracked.erase(position_id);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

std::vector<std::string> StopLossEngine::evaluate(
    const std::unordered_map<std::string, Position> &current_positions,
    Timestamp current_time)
{
    // Evaluate all tracked positions against stop conditions:
    // 1. Acquire exclusive lock (updates trailing best_price)
    // 2. For each tracked position:
    //    a. Look up in current_positions; skip if missing (already closed)
    //    b. Update trailing best_price if applicable
    //    c. Check stop condition by mode
    //    d. Add triggered IDs to result

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    std::vector<std::string> triggered;

    for (auto &[pos_id, tracked] : m_tracked)
    {
        // a. Look up current position.
        auto it = current_positions.find(pos_id);
        if (current_positions.end() == it)
        {
            continue; // Position no longer exists.
        }

        const auto &pos = it->second;
        const Price current = pos.current_price;

        // b. Update trailing best_price.
        if (Side::Buy == tracked.side && current > tracked.best_price)
        {
            tracked.best_price = current;
        }
        else if (Side::Sell == tracked.side && current < tracked.best_price)
        {
            tracked.best_price = current;
        }

        // c. Check stop condition by mode.
        bool should_trigger = false;

        switch (tracked.config.mode)
        {
          case StopMode::Fixed:
          {
              // Buy: trigger when current <= entry * (1 - fixed_pct).
              // Sell: trigger when current >= entry * (1 + fixed_pct).
              if (Side::Buy == tracked.side)
              {
                  const Price stop_level = tracked.entry_price * (1.0 - tracked.config.fixed_pct);
                  if (current <= stop_level)
                  {
                      should_trigger = true;
                  }
              }
              else
              {
                  const Price stop_level = tracked.entry_price * (1.0 + tracked.config.fixed_pct);
                  if (current >= stop_level)
                  {
                      should_trigger = true;
                  }
              }
              break;
          }

          case StopMode::Trailing:
          {
              // Buy: trigger when current <= best * (1 - trailing_pct).
              // Sell: trigger when current >= best * (1 + trailing_pct).
              if (Side::Buy == tracked.side)
              {
                  const Price trail_level = tracked.best_price * (1.0 - tracked.config.trailing_pct);
                  if (current <= trail_level)
                  {
                      should_trigger = true;
                  }
              }
              else
              {
                  const Price trail_level = tracked.best_price * (1.0 + tracked.config.trailing_pct);
                  if (current >= trail_level)
                  {
                      should_trigger = true;
                  }
              }
              break;
          }

          case StopMode::TimeBased:
          {
              // Trigger when elapsed time >= max_hold_seconds.
              const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                  current_time - tracked.open_time).count();

              if (elapsed >= static_cast<std::int64_t>(tracked.config.max_hold_seconds))
              {
                  should_trigger = true;
              }
              break;
          }
        }

        if (should_trigger)
        {
            PULSE_LOG_INFO("risk",
                "Stop-loss triggered: {} mode={} current={:.2f}",
                pos_id, static_cast<int>(tracked.config.mode), current);
            triggered.push_back(pos_id);
        }
    }

    return triggered;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool StopLossEngine::isTracked(const std::string &position_id) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return m_tracked.count(position_id) > 0;
}

std::size_t StopLossEngine::trackedCount() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return m_tracked.size();
}

} // namespace pulse::risk
