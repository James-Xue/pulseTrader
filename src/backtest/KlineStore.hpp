#pragma once
// kline_store.hpp — Trailing kline accumulation + DB import (M33, A3)
//
// Gate REST serves at most the most-recent ~10000 1m points per symbol
// (≈6.9 days), so a backtest window longer than that needs a history store
// that accumulates every day. KlineStore is that store: it fetches the
// trailing window from an IKlineSource (GateKlineFetcher), drops the forming
// candle, and merges the rest into an IKlineSource sink (SqliteKlineReader,
// INSERT OR IGNORE — idempotent, so outages ≤ ~6.9 days self-heal on the
// next run). importFuturesFromDb jump-starts the store from an existing
// kline_bars database (e.g. the local/VPS trades.db with months of rows).

#include "backtest/KlineSource.hpp"
#include "core/PulseError.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// KlineStoreStats — what one store pass produced
// ---------------------------------------------------------------------------
struct KlineStoreStats
{
    std::size_t rows_fetched = 0; ///< Candles returned by the source.
    std::size_t rows_new = 0;     ///< Candles actually inserted (after dedup).
    std::int64_t first_open_ms = 0;
    std::int64_t last_open_ms = 0;
};

// ---------------------------------------------------------------------------
// KlineStore — fetch-trailing + merge-into-sink, one symbol per call
// ---------------------------------------------------------------------------
class KlineStore
{
  public:
    /// `api` is the fetch source (GateKlineFetcher), `sink` the persistence
    /// target (SqliteKlineReader). KlineStore owns neither.
    KlineStore(IKlineSource &api, IKlineSource &sink);

    /// Fetch [from_ms, to_ms], drop candles that have not finished yet
    /// (open_time >= now_ms - 60s), write the rest into the sink.
    /// On a fetch failure the window retries once shifted +10 min forward
    /// (Gate's dynamic "most recent 10000 points" boundary), mirroring
    /// DailyKlineSync. Returns per-run stats.
    [[nodiscard]] Result<KlineStoreStats> storeWindow(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms, std::int64_t now_ms);

  private:
    IKlineSource &m_api;
    IKlineSource &m_sink;
};

/// Copy all futures kline_bars rows (optionally only `symbols`) from another
/// database into `sink`. Reads work on any schema version (v1 or v2) — only
/// the sink's INSERT OR IGNORE dedup matters. Returns the number of rows
/// inserted. Callers on non-SQLite builds cannot link this TU.
[[nodiscard]] Result<std::size_t> importFuturesFromDb(
    const std::string &source_db_path,
    const std::vector<std::string> &symbols,
    IKlineSource &sink);

} // namespace pulse::backtest
