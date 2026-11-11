#pragma once
// gold_strategy_engine.hpp — strategy engine for CFD gold (XAUUSD) long entries
//
// The "启动引擎" button starts a polling loop that feeds every fresh price
// into this engine; the engine decides whether to open a long position and
// computes the take-profit / stop-loss prices.
//
// v1 strategy (trend + volatility envelope, deliberately simple and
// replaceable):
//   1. Trend: EMA(fast) vs EMA(slow) of the polled close prices — a long is
//      only eligible while the fast line is above the slow line.
//   2. Volatility: peak-to-trough range of the last `volWindow` ticks
//      (a tick-level ATR stand-in — the poll only sees last prices, no
//      OHLC bars).
//   3. Entry: market price at decision time. TP = entry + tpVolMult×vol,
//      SL = entry − slVolMult×vol (both bounded by min/max volatility).
//   4. Cooldown: after one open signal, no repeat signal for `cooldownMs`.
//
// Thread safety: not thread-safe — call onPrice() from a single thread (the
// polling thread). Pure C++/QtCore, no UI dependency → unit-testable.

#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace pulse::ui
{

/// One strategy evaluation outcome.
struct GoldDecision
{
    bool shouldOpen = false;      ///< Open a long position at entryPrice.
    double entryPrice = 0.0;      ///< Suggested entry (current market price).
    double takeProfit = 0.0;      ///< TP price (entry + tpVolMult×vol).
    double stopLoss = 0.0;        ///< SL price (entry − slVolMult×vol).
    double volumeLots = 0.0;      ///< Suggested open size (lots, XAUUSD min 0.01).
    QString reason;               ///< Human-readable decision reason (Chinese, shown in the UI).
};

class GoldStrategyEngine
{
  public:
    /// Tuning parameters. Every field is atomic so the UI thread can hot-
    /// update them while the polling thread evaluates (no locks needed).
    /// The engine holds the Params via shared_ptr — safe even if the owning
    /// window is destroyed.
    struct Params
    {
        std::atomic<int> emaFast{ 10 };           ///< Fast EMA period (ticks).
        std::atomic<int> emaSlow{ 30 };           ///< Slow EMA period (ticks).
        std::atomic<int> volWindow{ 20 };         ///< Ticks used for the volatility range.
        std::atomic<double> tpVolMult{ 1.5 };     ///< TP = entry + mult × volatility.
        std::atomic<double> slVolMult{ 1.0 };     ///< SL = entry − mult × volatility.
        std::atomic<double> tpPoints{ 5.0 };      ///< Fixed-point TP distance (SuperTrend mode).
        std::atomic<double> slPoints{ 5.0 };      ///< Fixed-point SL distance (SuperTrend mode).
        std::atomic<double> minVolUsd{ 0.5 };     ///< Volatility floor (USD) — silence noise.
        std::atomic<double> maxVolUsd{ 50.0 };    ///< Volatility ceiling (USD) — reject spikes.
        std::atomic<std::int64_t> cooldownMs{ 60'000 };  ///< Min gap between open signals.
        std::atomic<double> openVolumeLots{ 0.01 };      ///< Open size (lots) per signal.
        std::atomic<double> openLeverage{ 1.0 };         ///< Simulated margin leverage (real-CFD model).
        std::atomic<int> openProtectMin{ 10 };           ///< Minutes after market open with entries blocked.
        // Real-position auto-follow (throttled PUT of live CFD stops).
        std::atomic<bool> autoFollowReal{ false };       ///< Master switch (default OFF — real writes).
        std::atomic<int> followIntervalSec{ 30 };        ///< Min seconds between follow PUTs.
        std::atomic<double> followThresholdPoints{ 2.0 };///< Min price move to trigger a follow.
    };

    explicit GoldStrategyEngine(
        std::shared_ptr<const Params> params = std::make_shared<Params>());

    /// Feed one observed price and evaluate the strategy.
    ///
    /// Parameters:
    ///   1. price — XAUUSD last price (USD)
    ///   2. nowMs — Unix millisecond timestamp (used for the cooldown)
    GoldDecision onPrice(double price, std::int64_t nowMs);

    /// Drop all history (used on engine restart).
    void reset();

    /// Number of prices fed so far (diagnostics/tests).
    int priceCount() const;

    /// Suggested stop prices for the current market state (trailing stops).
    ///
    /// Recomputes the volatility envelope against `price` the same way the
    /// open decision does: tp = price + tpVolMult×vol, sl = price −
    /// slVolMult×vol. Returns nullopt while still warming up or when the
    /// volatility is out of bounds. The caller (SimTradeEngine::updateStops)
    /// applies the suggestion with a move-only-favourable constraint.
    struct GoldStops
    {
        double takeProfit = 0.0;
        double stopLoss = 0.0;
    };
    std::optional<GoldStops> suggestStops(double price) const;

  private:
    /// Peak-to-trough volatility of the last volWindow ticks; 0.0 when the
    /// window has not filled yet.
    double volatility() const;
    double ema(int period) const;

    std::shared_ptr<const Params> m_params;
    std::vector<double> m_prices;        ///< Ring-ish buffer, capped.
    std::int64_t m_lastSignalMs = 0;     ///< Last open-signal timestamp.
};

} // namespace pulse::ui
