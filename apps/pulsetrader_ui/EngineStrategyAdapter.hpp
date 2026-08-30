#pragma once
// engine_strategy_adapter.hpp — bridge the engine's real strategy into the
// launcher polling loop
//
// The engine's gold-capable strategies (EmaResonanceScalper and the
// UnifiedScalper family) expose a pure-function entry point:
//
//     evaluateEntry(const std::vector<market::Kline>&) → EntryContext
//
// which computes the full indicator stack from a kline snapshot with no
// MarketFeed/OrderExecutor dependency. This adapter drives that entry point
// from the launcher's tick poll:
//
//   1. warmup(): backfills 1m klines via GET /tradfi/symbols/XAUUSD/klines
//      (the strategy needs ~201 candles for its EMA seed)
//   2. onTick(): aggregates the polled last price into a 1m candle and
//      re-evaluates the strategy every tick
//   3. A Buy entry yields the decision: entry = current price, TP/SL =
//      entry ± mult × ATR(14) — the same envelope idea as the old v1 engine,
//      now driven by the engine's real strategy.
//
// Only long (Buy) entries are surfaced — the launcher paper-trades longs.
//
// Thread safety: NOT thread-safe — call from the single polling thread.

#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pulse::exchange
{
class GateRestClient;
}

namespace pulse::market
{
struct Kline;
}

namespace pulse::ui
{

// Adapter-local subclass that lifts EmaResonanceScalper's protected
// evaluateEntry() to public (defined in the .cpp; unique_ptr needs only the
// declaration here).
class ExposedResonance;

/// One adapter evaluation outcome (the launcher-facing decision).
struct AdapterDecision
{
    bool shouldOpen = false;
    double entryPrice = 0.0;
    double takeProfit = 0.0;
    double stopLoss = 0.0;
    double confidence = 0.0;
    QString reason;
};

class EngineStrategyAdapter
{
  public:
    EngineStrategyAdapter();
    ~EngineStrategyAdapter();

    /// Backfill 1m kline history for warm-up (blocking REST call — run off
    /// the UI thread). Returns the number of candles loaded (0 on failure;
    /// the strategy then warms up organically from live polls).
    int warmup(pulse::exchange::GateRestClient &cfd);

    /// Feed one observed price and evaluate the strategy.
    /// Returns nullopt while warming up or when no Buy entry triggers.
    std::optional<AdapterDecision> onTick(double price, std::int64_t nowMs,
                                          double tpMult, double slMult,
                                          std::int64_t cooldownMs);

    /// Trailing-stop envelope: TP/SL = price ± mult × ATR14 from the latest
    /// candle. `tpMult`/`slMult` hot-read by the caller each round. Returns
    /// nullopt while warming up.
    struct Stops
    {
        double takeProfit = 0.0;
        double stopLoss = 0.0;
    };
    std::optional<Stops> suggestStops(double price, double tpMult, double slMult) const;

    /// Candles buffered so far (diagnostics/tests).
    int klineCount() const;

  private:
    std::vector<pulse::market::Kline> m_klines;   ///< 1m candles, capped.
    std::int64_t m_currentBucketMs = -1;          ///< Minute bucket of the open candle.
    std::unique_ptr<ExposedResonance> m_strategy;
    std::int64_t m_lastSignalMs = 0;
};

} // namespace pulse::ui
