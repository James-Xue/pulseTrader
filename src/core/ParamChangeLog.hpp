#pragma once
// param_change_log.hpp — ring-buffer audit log for strategy parameter changes
//
// Both write paths (the AI pipeline and the manual set_strategy_param
// control-plane channel) record every parameter mutation here so operators
// can answer "who changed what, when, and from what to what". A fixed-capacity
// ring keeps the memory bounded; the newest entries are the ones operators
// actually chase.
//
// Zero dependencies on other modules — lives in core so ai / control /
// heartbeat can all reference it without new link edges.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::core
{

struct ParamChangeEntry
{
    std::int64_t ts_ns{ 0 };    ///< Audit timestamp (epoch ns).
    std::string strategy_id;    ///< Strategy instance id, e.g. "momentum_scalper_BTC_USDT".
    std::string param_name;     ///< Parameter key, e.g. "min_confidence".
    double old_value{ 0.0 };
    double new_value{ 0.0 };
    std::string source;         ///< "ai" (pipeline) | "manual" (set_strategy_param).
};

/// Fixed-capacity ring buffer, mutex-guarded. New entries evict the oldest.
class ParamChangeLog
{
  public:
    explicit ParamChangeLog(std::size_t capacity = 256);

    /// Record one parameter change. Thread-safe.
    void record(ParamChangeEntry entry);

    /// Snapshot of the log, newest first. Thread-safe.
    [[nodiscard]] std::vector<ParamChangeEntry> snapshot() const;

    /// Number of entries currently held (≤ capacity).
    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] std::size_t capacity() const;

  private:
    mutable std::mutex m_mutex;
    const std::size_t m_capacity;
    std::vector<ParamChangeEntry> m_entries; ///< Oldest at index 0.
};

} // namespace pulse::core
