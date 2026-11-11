#pragma once
// engine_strategy_adapter.hpp — bridge the engine's real strategy into the
// launcher polling loop
//
// The engine's gold strategies (UnifiedScalper family) expose pure-function
// entry points that compute from a kline snapshot with no MarketFeed
// dependency. This adapter drives the SuperTrend strategy from the
// launcher's tick poll across FOUR timeframes:
//
//   tracks:  10s · 1m · 15m · 30m
//   warmup:  backfills each timeframe via GET /tradfi/symbols/XAUUSD/klines
//            (kline_type per timeframe, 500 candles)
//   onTick:  aggregates the polled last price into each timeframe's candle
//            and evaluates the SuperTrend state every pass
//   signal:  the 1m timeframe's flip bear→bull is the BUY entry (entry =
//            current price, TP/SL = entry ± mult × ATR)
//   display: every pass exposes all four timeframes' line value + direction
//            (10s/1m/15m/30m) for the trend-readout line
//
// Only long (Buy) entries are surfaced — the launcher paper-trades longs.
//
// Thread safety: NOT thread-safe — call from the single polling thread.

#include <QString>

#include <nlohmann/json.hpp>

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

namespace pulse::strategy
{
class SuperTrendScalper;
}

namespace pulse::ui
{

/// One adapter evaluation outcome (the launcher-facing decision).
struct AdapterDecision
{
    bool shouldOpen = false;
    QString side;            ///< "long" (bear→bull flip) or "short" (bull→bear flip).
    double entryPrice = 0.0;
    double takeProfit = 0.0; ///< Fixed-point envelope: entry ± tpPoints.
    double stopLoss = 0.0;   ///< entry ∓ slPoints (opposite side for shorts).
    double confidence = 0.0;
    QString reason;
};

/// Current SuperTrend line state for one timeframe (trend readout).
struct TrendInfo
{
    QString label;         ///< "10s" / "1m" / "15m" / "30m".
    double supertrend = 0.0;
    bool bullish = false;  ///< True = uptrend (开多).
    bool valid = false;    ///< False while the timeframe is still warming up.
};

/// Full outcome of one poll round.
struct TickOutcome
{
    std::optional<AdapterDecision> decision;  ///< 1m flip-to-bull Buy entry.
    std::vector<TrendInfo> trends;            ///< All four timeframes (fixed order).
    bool trendChanged = false;                ///< Any timeframe's direction changed.
};

class EngineStrategyAdapter
{
  public:
    EngineStrategyAdapter();
    ~EngineStrategyAdapter();

    /// Backfill kline history for every timeframe (blocking REST calls —
    /// run off the UI thread). Returns the total candles loaded (0 on total
    /// failure; timeframes then warm up organically from live polls).
    int warmup(pulse::exchange::GateRestClient &cfd);

    /// Feed one observed price, aggregate into each timeframe, evaluate the
    /// SuperTrend state, and (on the 1m flip) produce a Buy/Sell entry:
    ///   bear → bull  = long (side "long")
    ///   bull → bear  = short (side "short")
    ///
    /// Parameters:
    ///   1. price — XAUUSD last price
    ///   2. nowMs — Unix millisecond timestamp
    ///   3. tpPoints / slPoints — fixed-point envelope: long TP/SL =
    ///      price ± points; short mirrored (hot-read by caller)
    ///   4. cooldownMs — min gap between open signals
    TickOutcome onTick(double price, std::int64_t nowMs, double tpPoints,
                       double slPoints, std::int64_t cooldownMs);

    /// 1m candles buffered so far (diagnostics/tests).
    int klineCount() const;

    /// Parse one kline object into a candle (public for unit testing).
    ///
    /// Gate TradFi klines use the compact schema {o,h,l,c,t} with t in Unix
    /// SECONDS; full-name fields ({open,high,low,close} + timestamp/open_time)
    /// are accepted as a fallback for other Gate products. Returns false when
    /// the required fields are missing.
    static bool parseKline(const nlohmann::json &obj, pulse::market::Kline &out);

  private:
    /// One timeframe track: candle aggregation + its own SuperTrend instance.
    struct Track
    {
        std::int64_t periodMs = 60'000;
        QString label;
        std::vector<pulse::market::Kline> klines;
        std::unique_ptr<pulse::strategy::SuperTrendScalper> strategy;
        std::int64_t currentBucketMs = -1;
        double lastSupertrend = 0.0;
        double lastAtr = 0.0;      ///< ATR of the latest evaluation (trailing envelope).
        bool lastBullish = false;
        bool hasState = false;
    };

    std::vector<Track> m_tracks;      ///< 10s · 1m · 15m · 30m (fixed order).
    std::int64_t m_lastSignalMs = 0;
};

} // namespace pulse::ui
