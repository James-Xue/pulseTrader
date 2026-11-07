#pragma once
// kline_loader.hpp — Orchestrates candle loading for a backtest run (M29)
//
// Pipeline:
//   1. SQLite first — read the requested window from kline_bars (local truth)
//   2. Gap detection — find [from,to] sub-ranges with no local candles
//   3. API backfill — fetch each gap from the exchange (when enabled)
//   4. Merge — sort by open_time, dedup (local rows win)
//   5. Cache write-back — persist API-fetched candles (INSERT OR IGNORE)
//
// sqlite may be nullptr (build without PULSE_ENABLE_SQLITE or no local DB):
// the loader then degrades to a pure API fetch.

#include "backtest/KlineSource.hpp"
#include "backtest/backtest_types.hpp"

#include <cstdint>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// KlineLoader — SQLite-first, API-gap-fill orchestration
// ---------------------------------------------------------------------------
class KlineLoader
{
  public:
    /// `sqlite` may be null; `api` is always required (may be unused when
    /// api_backfill is false or the local data covers the whole window).
    /// Both are injected as IKlineSource so tests can stub the exchange.
    explicit KlineLoader(IKlineSource *sqlite, IKlineSource &api);

    /// Load candles for the requested window. Fills `stats` with provenance
    /// (local vs API rows) and any warnings collected along the way.
    ///
    /// Returns BacktestNoCandles when the load produced zero candles.
    [[nodiscard]] Result<std::vector<market::Kline>> load(
        const KlineLoadRequest &req, KlineLoadStats &stats);

  private:
    IKlineSource *m_sqlite; ///< Nullable.
    IKlineSource &m_api;
};

} // namespace pulse::backtest
