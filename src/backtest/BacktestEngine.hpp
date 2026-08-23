#pragma once
// backtest_engine.hpp — Top-level orchestration for one backtest run (M29)
//
// Pipeline (see KlineLoader for the data side):
//   options (CLI) → optional trading.toml instance seeding
//   → resolve the time window (explicit, or from local coverage / default)
//   → KlineLoader.load (sqlite first, Gate API gap-fill, cache write-back)
//   → ReplayDriver.run (offline strategy replay)
//   → window-end flattening → BacktestReport (table + optional JSON export)

#include "backtest/backtest_types.hpp"
#include "core/PulseError.hpp"

#include <string>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// BacktestEngine — executes one backtest and produces the report text
// ---------------------------------------------------------------------------
class BacktestEngine
{
  public:
    explicit BacktestEngine(BacktestOptions opts);

    /// Run the full pipeline. Returns the formatted report text (stdout
    /// table); writes the JSON export when opts.json_export_path is set.
    [[nodiscard]] Result<std::string> run();

  private:
    BacktestOptions m_opts;
};

} // namespace pulse::backtest
