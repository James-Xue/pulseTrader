#pragma once
// warmup_seeder.hpp — Startup kline preload into feed buffers (M30)
//
// The engine used to warm strategies up by accumulating live WS kline
// pushes — EmaResonance needs 201 candles ≈ 3.4h. Gate REST serves live
// futures candles (~30s fresh), so on startup we can instead seed each
// feed's KlineBuffer with the recent history and let the strategy's first
// poll complete warmup instantly, then continue on live pushes.
//
// Contract:
//   - Must run on the main thread, strictly BEFORE feed->start() (the
//     KlineBuffer assumes a single writer; the WS I/O thread starts writing
//     once the feed starts).
//   - Never throws, never blocks startup: a failing symbol degrades to the
//     traditional live warmup with a WARN log.
//   - Never writes back to kline_bars (cache_writeback=false) — self-recorded
//     futures klines are being retired in M30.

#include "backtest/KlineLoader.hpp"
#include "core/types.hpp"
#include "market/MarketFeed.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// WarmupSeeder — seed historical closed candles into feed KlineBuffers
// ---------------------------------------------------------------------------
class WarmupSeeder
{
  public:
    /// Same source shape as KlineLoader: `sqlite` may be null (no SQLite
    /// build or no local DB); `api` is always required.
    explicit WarmupSeeder(IKlineSource *sqlite, IKlineSource &api);

    /// For each symbol, load the trailing `count` (default 500 = KlineBuffer
    /// capacity) 1m candles and push them (ascending open_time, closed=true)
    /// into `feed.getKlineBuffer(symbol)`. Returns the total number of
    /// candles seeded across all symbols.
    [[nodiscard]] std::size_t seed(market::MarketFeed &feed,
                                   const std::vector<std::string> &symbols,
                                   MarketType market_type,
                                   std::size_t count = 500);

  private:
    KlineLoader m_loader;
};

} // namespace pulse::backtest
