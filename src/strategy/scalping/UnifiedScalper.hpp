#pragma once
// unified_scalper.hpp — Template-method base for kline-driven scalpers (Layer 6)
//
// Converges the repeated scaffolding of the kline-driven strategies
// (MomentumScalper / MeanReversionScalper / SuperTrendScalper) into one base:
//   - onKline() / onTick() are final — the evaluation order below is locked in
//     (order equivalence proof for the migration; subclasses override hooks,
//     not the template):
//       1. feed null  → return
//       2. snapshot(klineNeeded()) from the KlineBuffer
//       3. candles < warmupThreshold() → throttled warmup log, return
//       4. evaluateEntry(candles) — detection + rolling-state commit
//       5. nullopt → return (state already committed per subclass rules)
//       6. inCooldown() → return (state already committed, timestamp untouched)
//       7. buildSignal + base fills symbol / strategy_id / timestamp
//       8. logSignal → emitSignal → m_lastSignalTimeMs = now
//   - computeAtr() — one copy instead of three verbatim duplicates
//   - cooldown / warmup / no-data throttled logging — one implementation
//
// Behavior contract:
//   - id() = idPrefix() + "_" + config.symbol. The id is the SignalBoard key
//     and the address used by get_strategy_params / get_signals consumers —
//     subclasses must return their legacy prefix verbatim.
//   - evaluateEntry commits rolling state on every call except the documented
//     early-exits (e.g. SuperTrend's atr<=0 bail); cooldown blocking must NOT
//     undo an already-committed state (legacy SuperTrend behavior).
//   - The default evaluateEntry returns nullopt — UnifiedScalper is concrete
//     so the StrategyRegistry can use a passive instance as the fallback for
//     unknown strategy names (never emits signals).
//
// Thread safety:
//   - Runs on its own std::jthread (started by StrategyManager)
//   - State fields are only written from the strategy thread
//   - m_params is atomic (inherited from StrategyParams)

#include "strategy/StrategyBase.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// EntryContext — outcome of one evaluation pass (subclass hook output)
// ---------------------------------------------------------------------------
struct EntryContext
{
    SignalType type{ SignalType::Flat }; ///< Signal direction.
    double price{ 0.0 };                 ///< Reference price (latest close).
    double confidence{ 0.0 };            ///< Confidence computed by the subclass.
    double atr{ 0.0 };                   ///< ATR used for normalization (0 if n/a).
    std::string reason;                  ///< Human-readable signal reason.
    nlohmann::json indicators;           ///< Indicator snapshot (board/debug).
};

// ---------------------------------------------------------------------------
// UnifiedScalper — template-method base for kline-driven scalpers
// ---------------------------------------------------------------------------
class UnifiedScalper : public StrategyBase
{
  public:
    /// Construct with injected context (same semantics as the legacy ctors:
    /// stores the context for the hooks).
    explicit UnifiedScalper(const StrategyContext &context);

    // --- StrategyBase overrides (final: template locked) ---

    [[nodiscard]] std::string name() const override final;
    [[nodiscard]] std::string id() const override final;
    [[nodiscard]] StrategyParams &params() override final;

    /// Called on each ticker update — kline-driven: used only to detect
    /// "no kline data at all" (e.g. WS not connected), throttled to 30 s.
    void onTick(const market::Ticker &ticker) override final;

    /// Called on each closed K-line candle — the template method.
    void onKline(const market::Kline &kline) override final;

    /// Called on orderbook updates — not used (kline-driven); subclasses
    /// with a different data source (e.g. OrderBookScalper) stay outside
    /// this hierarchy.
    void onOrderbook(const market::OrderBook &book) override;

  protected:
    // --- Extension hooks (subclass override points) ---

    /// Strategy class name; default "UnifiedScalper" (used by name()).
    [[nodiscard]] virtual std::string className() const;

    /// ID prefix; default = config.name (so a fallback instance's id
    /// follows the TOML name — "bogus_scalper_BTC_USDT").
    [[nodiscard]] virtual std::string idPrefix() const;

    /// How many candles to pull from the KlineBuffer for one evaluation.
    [[nodiscard]] virtual std::size_t klineNeeded() const;

    /// Warmup threshold — below this candle count the template only logs.
    /// Defaults to klineNeeded(); Momentum overrides (needs slow_period+1
    /// candles but signals only after slow_period).
    [[nodiscard]] virtual std::size_t warmupThreshold() const;

    /// Whether the cooldown gate applies. Momentum never had one (its
    /// cooldown_seconds default is 30 but the legacy code never checked it)
    /// → returns false there. Cooldown is also disabled when
    /// cooldown_seconds <= 0.
    [[nodiscard]] virtual bool cooldownEnabled() const;

    /// Detection + rolling-state commit. Must commit state even when no
    /// signal fires (legacy SuperTrend commits unconditionally after the
    /// flip check); documented early-exits (atr<=0) must NOT commit.
    /// Returns nullopt when no entry this pass.
    virtual std::optional<EntryContext> evaluateEntry(
        const std::vector<market::Kline> &candles);

    /// Assemble the TradingSignal from an entry (default: pure copy of the
    /// entry fields; base fills symbol / strategy_id / timestamp).
    [[nodiscard]] virtual TradingSignal buildSignal(const EntryContext &entry) const;

    /// Signal log line; default: "confidence={:.4f}, price={:.2f}" (the
    /// legacy Momentum format). Subclasses with richer lines override.
    virtual void logSignal(const TradingSignal &sig) const;

    // --- Shared utilities (concrete, not virtual) ---

    /// ATR over the last `period` candles. TR = max(high-low, |high-prev_close|,
    /// |low-prev_close|). Returns 0.0 when fewer than period+1 candles.
    [[nodiscard]] double computeAtr(const std::vector<market::Kline> &candles,
        std::size_t period) const;

    /// EMA from a series of close prices.
    ///
    /// EMA = price * k + prev_ema * (1 - k), where k = 2 / (period + 1).
    /// On first call (prev_ema == 0.0) seeds with SMA of the first `period`
    /// closes. Returns 0.0 for an empty series.
    [[nodiscard]] static double computeEma(const std::vector<double> &closes,
        double period,
        double prev_ema);

    /// Coin-specific parameter lookup (from the TOML instance-level
    /// `custom_params` inline table). Static — read once at construction;
    /// returns `fallback` when the key is not configured.
    [[nodiscard]] double customParam(const std::string &key, double fallback) const;

    /// Current wall-clock time in ms since epoch.
    [[nodiscard]] std::int64_t nowMs() const;

    /// Cooldown gate: disabled when cooldownEnabled() is false or
    /// cooldown_seconds <= 0; otherwise true within cooldown_seconds of the
    /// last emitted signal.
    [[nodiscard]] bool inCooldown() const;

    /// "Warming up: have/need candles" log, throttled to 30 s.
    void logWarmupThrottled(std::size_t have, std::size_t need);

    /// "Waiting for kline data" log, throttled to 30 s.
    void logNoDataThrottled();

    // --- Shared state ---

    StrategyParams m_params;              ///< Hot-reloadable parameters.
    std::int64_t m_lastSignalTimeMs{ 0 }; ///< Last emitted signal time (cooldown).
    std::int64_t m_lastWarmupLogMs{ 0 };  ///< Throttle warmup log to every 30 s.
    std::int64_t m_lastNoDataLogMs{ 0 };  ///< Throttle "no data" log to every 30 s.
};

} // namespace pulse::strategy
