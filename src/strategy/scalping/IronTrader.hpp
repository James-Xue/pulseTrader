#pragma once
// iron_trader.hpp — IronTrader 铁律交易员 (Layer 6; rule baseline:
// docs/strategies/iron-trader.md — the doc is the SINGLE source of truth,
// this file mirrors it; any behavioural change starts in the doc).
//
// A "real trader" persona encoded as a rule stack: low-frequency, high-
// conviction trend following on 1m candles under a daily iron-rule regime.
//
// Lifecycle — fully managed INSIDE the strategy (the M33 account flip model
// alone cannot express "exit without re-entering"):
//   Flat ──(T-A pullback | T-B confirmed breakout, all gates green)──▶ Long/Short
//   Long/Short ──(hard stop / structure flip / momentum flip / time stop)──▶ Flat
// Every exit emits a *Flat* signal — a close-only channel: BacktestAccount
// closes without opening the opposite side; the signal board / aggregator
// treat Flat as a status event as usual. Live OrderFlowExecutor semantics for
// this strategy are future work (strategy runs signal-only today).
//
// Gate stack per candle (rules doc §4):
//   G1 structure — close vs trend EMA + 20-bar slope; no trend → stand aside
//   G2 momentum   — sep = (ema_fast − ema_slow)/ATR14 beyond the gate
//                   (×it_tight_gate_mult in the tight session 20:00–06:00Z,
//                    ×it_streak_gate_mult while the R6 redemption quota is
//                    active — 连亏 2 后"最强信号")
//   G3 volatility — regime != hot; spike roots (TR > 3×medTR20) force
//                   re-confirmation; maxTR20 > it_spike_skip_mult × stop
//                   distance skips the entry (R5)
// Trigger: T-A 拉回 (previous close under EMA_fast, current close reclaims it)
//          T-B 突破 (close breaks the N-bar box + ≥2 consecutive closes
//          outside; a spike root inside the window demands one extra NORMAL
//          bar outside — 回合 446 复证). RSI overbought/oversold blocks T-B
//          chases only (§4.2).
//
// Iron rules R0–R7 (§3): one position at a time (R2); stop cost ≤ it_risk_pct
// of day-start equity or NO signal (R1); realized daily loss ≥ it_day_loss_pct
// stops the day together with the consecutive-loss and per-day trade caps
// (R6); breakeven then trailing stop, never back (R7).
//
// Paper ledger [近似] — the strategy has no fill-feedback channel, so equity
// and realized PnL are ESTIMATES: quantity = atomic order_quantity, contract
// multiplier and taker fee approximated by it_quanto_est / it_fee_est.
// BacktestAccount's net PnL in the report is authoritative (§9).
//
// Exit model [近似]: M33 fills at candle close, so the hard stop / breakeven /
// trailing levels are evaluated against CLOSE prices, not intra-bar extremes
// (intra-bar fidelity is a documented future milestone).
//
// Warmup: klineNeeded = it_trend_ema + 60 (slope needs 20 bars past the SMA
// seed; +60 settles ATR/RSI). Deterministic full-series recompute every
// candle (same convention as EmaResonanceScalper) — no rolling drift.
//
// Wall-clock cooldown is disabled (cooldownEnabled → false): the trader rests
// in CANDLE terms (it_cooldown_bars); wall-clock cooldown would silently
// block exits under fast replay.

#include "strategy/scalping/UnifiedScalper.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulse::strategy
{

// ---------------------------------------------------------------------------
// IronTrader — 铁律交易员
// ---------------------------------------------------------------------------
class IronTrader : public UnifiedScalper
{
  public:
    using UnifiedScalper::UnifiedScalper;

  protected:
    // --- UnifiedScalper hooks ---

    [[nodiscard]] std::string className() const override;
    [[nodiscard]] std::string idPrefix() const override;
    [[nodiscard]] std::size_t klineNeeded() const override;
    [[nodiscard]] std::size_t warmupThreshold() const override;
    [[nodiscard]] bool cooldownEnabled() const override;
    std::optional<EntryContext> evaluateEntry(
        const std::vector<market::Kline> &candles) override;

  private:
    enum class Phase
    {
        Flat,   ///< No position; entries may be evaluated.
        Long,   ///< Virtual long position held.
        Short,  ///< Virtual short position held.
    };

    enum class StructDir
    {
        None,
        Bull,
        Bear,
    };

    struct Indicators
    {
        double atr14 = 0.0;
        double med_tr20 = 0.0;
        double max_tr20 = 0.0;
        bool hot = false;
        bool spike_near = false;  ///< Any of the last 3 TR > 3×medTR20.
        double ema_fast = 0.0;
        double ema_slow = 0.0;
        double ema_trend = 0.0;
        double trend_prev20 = 0.0;  ///< Trend EMA 20 candles ago.
        double sep = 0.0;           ///< (ema_fast − ema_slow) / ATR14.
        double rsi = 0.0;
        double box_high = 0.0;  ///< High of the last it_breakout_n CLOSED bars.
        double box_low = 0.0;
        double prev_close = 0.0;
        double prev_ema_fast = 0.0;
        bool tight_session = false;  ///< 20:00Z–06:00Z UTC (G2).
    };

    /// Custom-parameter read (shorthand for customParam).
    [[nodiscard]] double param(const std::string &key, double fallback) const;

    /// Effective |sep| gate for this candle (base × session × streak).
    [[nodiscard]] double sepGate(const Indicators &ind) const;

    /// Stop distance in price units = it_sl_atr × ATR14 (R = stop distance).
    [[nodiscard]] double stopDistance(const Indicators &ind) const;

    // --- Per-candle state machine pieces ---

    [[nodiscard]] Indicators computeIndicators(
        const std::vector<market::Kline> &candles) const;
    [[nodiscard]] StructDir structDir(const Indicators &ind, double close) const;

    /// Long/short exit handling while holding; commits the Flat exit when a
    /// rule fires (returns the Flat EntryContext). Also updates breakeven
    /// and the trailing stop (R7) when nothing fires.
    std::optional<EntryContext> managePosition(
        const market::Kline &cur, const Indicators &ind);

    /// Entry evaluation in the Flat phase (cooldown → gates → T-A/T-B).
    std::optional<EntryContext> evaluateEntrySetup(
        const std::vector<market::Kline> &candles, const Indicators &ind,
        StructDir struct_dir, double close);

    /// Commit a decided entry and build its Buy/Sell EntryContext.
    std::optional<EntryContext> finishEntry(double close,
        const Indicators &ind, bool bull, double sep_gate,
        const std::string &trigger, double sep_for_conf);

    /// R0/R1/R6 pre-trade checklist (30 秒检查表, §3). Logs the refusal.
    [[nodiscard]] bool riskChecklistPasses(const Indicators &ind);

    // --- Paper ledger (R0/R1/R6; USD estimates, see header note) ---

    struct DayCounters
    {
        std::int64_t day_id = 0;        ///< UTC day bucket (open_time ms / day).
        double day_realized_usd = 0.0;  ///< Estimated realized PnL this day.
        double equity_ref_usd = 0.0;    ///< R0: equity snapshot at day start.
        int trades_today = 0;
        int consec_losses = 0;
        bool day_stopped = false;       ///< R6 − it_day_loss_pct% or 3 losses.
    };

    void settleTrade(bool is_long, double entry, double exit);
    void rollDayIfNeeded(std::int64_t open_ms);
    [[nodiscard]] double estimateNetUsd(bool is_long, double entry,
        double exit);

    // --- Rolling state (strategy thread only) ---

    Phase m_phase{ Phase::Flat };
    double m_entry_price = 0.0;
    double m_stop_price = 0.0;        ///< Protective stop (moves one way only).
    double m_entry_atr = 0.0;         ///< ATR14 at entry.
    double m_best_close = 0.0;        ///< Best close since entry (trail base).
    bool m_breakeven_done = false;    ///< R7 stage: stop parked at cost.
    std::int64_t m_bars_held = 0;     ///< Candles since entry (time stop).
    std::int64_t m_flat_bars = 0;     ///< Candles since last exit (cooldown).

    // T-B breakout machine (Flat phase only).
    bool m_break_active = false;
    bool m_break_bull = false;        ///< Breakout side captured at the break bar.
    double m_break_level = 0.0;       ///< Box level being tested.
    double m_break_sep = 0.0;         ///< sep captured at the break bar.
    int m_break_run = 0;              ///< Consecutive closes beyond the level.

    DayCounters m_day;
    bool m_equity_initialised = false;
    double m_paper_equity_usd = 0.0;  ///< Running estimated equity (R0).
    bool m_after_streak_quota = false;///< R6: 连亏 2 → one redemption trade.
    bool m_quota_used = false;        ///< R6: redemption trade consumed.
};

} // namespace pulse::strategy
