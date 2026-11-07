# IronTrader — the iron-discipline trader (BTC USDT-M futures · 1m)

> A disciplined trend-following strategy that simulates a **real trader**: the human trader's "iron rules — checklist — review" self-discipline system, frozen into an executable, backtestable, auditable rule stack.
> Persona: the antithesis of high-frequency scalping — **better to miss a trade than to take a wrong one**. Signals are not "emitted because they were computed"; they are "the natural low frequency that survives stacked gates".
> This document is the **single rule baseline**; the C++ implementation (src/strategy/) must map to it clause by clause, with the mapping table (§9) backfilled.
> **Implementation mapping (2026-09-06)**: `src/strategy/scalping/IronTrader.{hpp,cpp}` (class `IronTrader` · registration key `iron_trader`); unit tests `tests/unit/strategy/test_iron_trader.cpp` (+ `BacktestAccountTest.FlatClosesPositionWithoutReopening`); all 1016 tests green.

---

## 0. Why a "trader" strategy is needed (empirical basis)

| # | Evidence | Conclusion | Source |
|---|---|---|---|
| E1 | M33 momentum × BTC 1m backtest over the recent week (2026-09-06): a 0.6 high-confidence gate = only 1 trade in 7 days, **+0.61**; a 0.05 low gate + 300s cooldown = 254 trades, **−50.25** (win rate 20.9%, ground down by fees) | Undisciplined frequent trading = steady losses; discipline = the edge itself | Backtest in this repository |
| E2 | M32 signal-log replay: chasing rallies in noisy conditions always loses; fast-layer mechanical risk control + slow-layer AI weight tuning is the correct shape | Price-chasing inside 1m noise has no alpha | Project memory |
| E3 | BTC 1m EMA-cross confidence is naturally only ~0.1–0.4 (ATR-normalised scale) | A single high "confidence" gate effectively disables trading on BTC → use **structural gates** (ATR-normalised separation, etc.) instead of mystical thresholds | Project memory |
| E4 | High-leverage liquidation-protection iron rules v2 (2026-08-30): all discipline converges to "compute the liquidation distance before entry; the stop-loss precedes liquidation and survives natively on the exchange" | Risk red lines = non-negotiable clauses | See §9 provenance |
| E5 | XAUUSD sub-agent live trading (08-17~20): the breakout confirmation rule accumulated **13 breakouts — 6 real, 7 false, a 100% separation** (no real breakout was reclaimed within 3 bars); the RSI hedge filter was validated 6 times over 3 windows on the long side and 2 times on the short side, with the price pulling back every time it blocked an entry; spike pre-check (skip when maxTR > 2×SL; round 446: a spike bar reclaimed the false breakout in one hit); capital gate (a thin account is exhausted by a single stop: a 10.68 account stopped out, went to 5.09 and stopped trading on the spot) | Each concrete rule form was falsified/confirmed in live trading | See §9 provenance |

**Design corollary**: a high gate with low frequency is profitable not because "the threshold number" works, but because **multi-layer structural filtering** does. IronTrader writes these layers out explicitly as gates.

---

## 1. Persona (why it behaves like a trader)

- **Role**: a BTC perpetual (USDT-margined) futures trader who executes on the 1m timeframe, but **forms the "big-picture read" for every trade from a longer-horizon structure**, and is not moved by 1m noise.
- **Character**: conservative, mechanical, averse to uncertainty. The day starts by passing a checklist; every order answers three questions: **Do I understand the direction? Is the stop-loss placeable? Can I survive the worst case?** If any answer is no, do not trade.
- **Daily routine** (UTC day boundary; the day rolls over at 08:00 Beijing):
  1. **Opening self-check**: confirm there are no abnormal leftover positions, that the daily counters are zeroed, and that the parameters were not changed;
  2. **Intraday loop**: evaluate once per 1m candle close (see the §6 state machine);
  3. **Post-close review**: log every close (entry/exit price, direction, R, reason, rule violations); when a red line triggers that day → log only and open no further positions;
  4. **Weekly review**: expectancy ≤ 0 or an abnormal run of losing days → a parameter-freeze review (isomorphic to the A/B experiment discipline, see docs/ab-weight-trial.md).

---

## 2. Market data and execution boundaries (state up front "what it can do")

| Item | Current state | Impact |
|---|---|---|
| Market data | This strategy only consumes **the instance symbol's 1m closed candles** (the engine's strategy callback normalises this); subscribing to real multi-timeframe candles is out of scope for this phase | Long-period EMAs / boxes are used to "simulate" the big picture on the same series — no pretending to have 15m/1h data |
| Backtest fill model | M33: signals trigger on a candle close and fill immediately at **the next close** (or the signal close — whichever the actual loader uses); intra-bar paths such as `stop_loss_pct/take_profit_pct` are **not implemented** | In backtests the SL/TP clauses reduce to "close-price-confirmed stop"; intra-bar fidelity → pending milestone (marked ⚠️ in §9) |
| Fees | Backtest default is futures taker 0.05%/leg (0.10% round trip) | High frequency is doomed; E2/E1 already prove it. IronTrader is low-frequency, but consider a maker variant when fee-sensitive |
| Capital model | The backtest account has **no initial capital / no liquidation / no funding fee** (incremental net PnL) | The "liquidation distance / equity percentage" clauses are semantically N/A in backtests; they remain live-trading clauses and are tagged [live] |
| Time events | Scheduled major news is handled by a hard gate on **preset UTC event windows** (§4.6, instance-level `news_windows`; injected in backtests via `--news`; same table, same behaviour); shocks not on the event list are still approximated by the volatility gate (§4.5) | **The strategy-side red-flag gate is implemented** (backtest + signal side, see §4.6); live flatten execution (OrderFlowExecutor consuming Flat) remains [live enhancement] |

---

## 3. Risk iron rules (red lines · immutable in any mode / any parameter version)

> Source: high-leverage liquidation-protection iron rules v2 + the XAU taskbook red lines. Changing any single one requires user review first and an entry in the revision log.

| # | Iron rule | Enforcement form in the engine/backtest |
|---|---|---|
| **R0** | **Equity basis**: the day-start equity is the account-equity snapshot at 00:00Z UTC, and every percentage that day uses it as the denominator. [backtest/paper] Paper equity = `it_equity_ref_usd` (default 100.0, the same order of magnitude as the VPS live account) + cumulative realised PnL, snapshotted at day start; [live] account equity (available + position equity), re-anchored to the latest snapshot after a deposit/withdrawal | Paper ledger built into the backtest; live trading uses the engine's equity basis ([live] note) |
| **R1** | **Per-trade stop cost ≤ 2% × day-start equity** (fix the stop distance first, then invert for size: size = equity×2% ÷ stop distance): stop cost = `it_sl_atr × ATR14 × quantity × quanto`; above 2% → **emit no signal** (checked before emission) | [live] the engine's risk gate backstops the order-size check; [backtest] paper-equity basis per R0 — over the limit the signal is rejected and flagged in the report |
| **R2** | **At most 1 position at a time**; hard notional-exposure cap | The strategy state machine "position open → exit management only"; the engine's maxOpenPositions backstops it with the same semantics |
| **R3** | **The stop-loss precedes liquidation**; the stop is exchange-native (futures price_orders) and never relies on engine memory | [live] place the exchange-side trigger order immediately on entry; [backtest] ⚠️ approximated by a close-confirmed stop |
| **R4** | **Averaging down into a losing position is forbidden**: no manual or automatic adding in the direction of a floating loss; only break-even and stop-trailing are allowed — never "buy more as it falls" | The strategy holds exactly 1 position + a cooldown, so averaging down is structurally impossible; the engine's reduce-only semantics backstop it |
| **R5** | **Volatility discipline**: `volatility_regime = hot` blocks new positions; when a spike bar (TR > 3×medTR20) occurred within the last 3 bars, a trigger must be **re-confirmed by a normal bar**; maxTR over the last 20 bars > 2× the SL distance → skip the entry (the slippage risk of a spike punching through the stop is unacceptable) | Fully backtestable (§4 volatility gate) |
| **R6** | **Daily circuit breaker = realised loss that day ≥ 5% × day-start equity → stop for the day** (the daily-loss red line **overrides everything** — after it is breached, that day is monitoring only; no exemption and no "strongest signal" reopens it); **3** consecutive losing trades → stop for the day; ≤ **6** entries per day; **after 2 consecutive losses the remaining daily allowance is 1 trade, and it must be the "strongest signal"** (sep ≥ `it_streak_gate_mult` × gate; source: the XAU rule "the next trade after 2 consecutive losses must be strongest-signal grade"; 16 rounds after FOMC with 0 opens and 0 closes) | Same semantics in backtest and live, reset on the UTC day boundary; counters go into the report |
| **R7** | **Profit protection**: floating profit ≥ 1 stop distance → move the stop to cost (break-even); thereafter only tighten, never loosen, trailing by ATR — **never let a floating profit turn into a floating loss** | §4 exit rules; M33 ⚠️ close-price approximation |

**Pre-order 30-second checklist** (executed mechanically before every signal emission):
1. R2: is there no open position? If there is one → exit management, no new entry
2. R5: do all volatility gates pass? (regime not hot / no unconfirmed spike / maxTR acceptable)
3. R6: are the daily counters within limits? (opened < 6 / consecutive losses < 3 / the day's realised loss has not reached −5% of day-start equity)
4. Does the direction agree with the structure gate? (§4, first layer)
5. If this is a breakout trigger: does the confirmation rule hold (two closes outside the boundary)? Did the RSI hedge filter not block it?
6. Is the stop level written down? How much is lost in the worst 8 hours (away from the screen)? Is that acceptable?

> All 6 pass → emit the signal; any single one fails → emit nothing this round and wait for the next close.

---

## 4. Trading rules

### 4.1 Three gate layers (entry evaluation is allowed only when all pass)

**G1 · Structure gate (the big picture — never trade against it)**: the relationship between price and the long EMA sets the stance for the day.
- **Bullish structure** = `close > EMA(it_trend_ema)` **and** the EMA is higher than 20 bars ago (trend up);
- **Bearish structure** = `close < EMA(it_trend_ema)` **and** the EMA is lower than 20 bars ago;
- Everything else (price tangled at the EMA / EMA flat) = **no structure → monitor only for the whole day**; this is the trend trader's "no trading in a range" discipline.

**G2 · Direction gate (momentum on the same side and strong enough)**:
```
sep = (EMA(fast) − EMA(slow)) / ATR14        // ATR normalisation, fixing the BTC 1m confidence distortion (E3)
bull momentum: sep ≥ +it_sep_gate      bear momentum: sep ≤ −it_sep_gate
```
- |sep| between 0 and the gate = a **convergence zone** (glued EMAs), the worst area for false crosses in the XAU evidence → no trade.
- **Session tiering** (BTC trades 24/7 with no maintenance window, so main/tight is only threshold tiering, not a trading switch): the main session **06:00Z–20:00Z** (European/US active hours) uses `it_sep_gate`; the tight session **20:00Z–06:00Z** (Asian session + thin overnight liquidity) multiplies the threshold by `it_tight_gate_mult` and adds `it_tight_cooldown_add` bars of cooldown (source: XAU taskbook v3.1 session-aware weights). Times are always judged on the UTC of the candle's **open_time** (consistent with the implementation; earlier drafts said close_time, see the v1.5 basis correction in the revision log) — never trust the local calendar for the day rollover (an empirical day-boundary lesson).

**G3 · Volatility gate (R5)**:
- `regime ≠ hot`: ATR14 ≤ it_regime_cap × median(TR, 20); over the limit = hot → no entries;
- If a spike bar (single-bar TR > 3×median(TR,20)) occurred within the last 3 bars → the entry trigger needs **one extra normal bar of re-confirmation** (guards against "one-hit reclaim" false breakouts/false reclaims; round 446 evidence);
- maxTR (last 20 bars) > it_spike_skip_mult × SL distance → skip this entry;
- **Spike-bar flagging** (review #9 convention): a single-bar TR > 3×medTR20 = a spike bar, flagged ⚠️ into `indicators.spike_flag`; recursive EMA/ATR cannot remove that bar cleanly → **honest approximation = do not exclude it from the computation, only require trigger re-confirmation and block entries** (full exclusion awaits an indicator-layer rewrite, see §9).

### 4.2 Triggers (after the two gate layers pass, choose one of two)

**T-A · Trend pullback (default pattern, preferred)**
- Long: bullish structure; the previous close < EMA(fast) (a pullback), **and this close reclaims above EMA(fast)** → the pullback is over and momentum has resumed: enter long; mirror for shorts.
- Semantics: follow the big picture, wait for a small retracement, and enter only once the resumption is confirmed — no chasing, no guessing.

**T-B · Breakout confirmation (secondary; requires G2 on the same side + the confirmation rule)**
- Long: the 1m close breaks the high of the last `it_breakout_n`-bar box **and the next close still holds above it** (breakout confirmation rule: filters false breakouts, evidenced by 4 false vs 1 real); mirror for shorts.
- **RSI hedge filter** (M22 lesson, rounds 130→132): when RSI(it_rsi_len) > it_rsi_ob (default 70), **do not chase a long breakout**; when RSI < it_rsi_os (default 30), **do not chase a short breakout**; this filter applies to T-B only — pullback T-A is unaffected.
- **Spike re-confirmation**: if a spike bar occurred within the last 3 bars, the confirmation rule needs, beyond the two confirming closes, one extra normal bar that is still outside the boundary (round 446).
- **Final pre-entry re-check** (round 178 lesson: the signal flipped 3 seconds after the decision, net −0.12): after T-A/T-B hold and before emitting the signal, check whether an **opposite** sep flip or structure flip just happened within the last `it_cooldown_bars` bars → if so, void this round. Backtests have no matching delay, so this check guards against the same noisy bar triggering in both directions.

### 4.3 Exits (while a position is open, each close evaluates exits only)

| Rule | Trigger | Action |
|---|---|---|
| **Hard stop** | The close (backtest approximation) / the native trigger order (live) reaches `entry ∓ it_sl_atr × ATR14` | Unconditional exit (R3) |
| **Break-even** | Floating profit ≥ `it_be_x_sl` × stop distance (default 1.0, i.e. the price retracing the whole stop distance) | Move the stop up to the entry price (R7) |
| **Trailing stop** | After break-even, stop = highest close − `it_trail_atr` × ATR14 (long); tighten only, never loosen | Locks in the floating profit (R7) |
| **Structure reversal** | The structure gate flips to the other side (the close breaks below the trend EMA and the EMA flattens/turns down) | Exit early instead of waiting for the stop |
| **Momentum reversal** | EMA(fast) crosses below EMA(slow) (for a long) | Exit early (the factor reversed; same as step 4 of the taskbook) |
| **Time stop** | Held > `it_timeout_bars` bars and floating profit < `it_timeout_x_sl` × stop distance | Leave at break-even/a small loss — do not "hold it hostage for research" too long |

> **Stop distance** (the unit in which this table measures PnL) = `it_sl_atr × ATR14`, i.e. how far the price must move to equal "one stop distance". Review convention: floating profits/losses are recorded as multiples of the stop distance (1.0 = the price travelled exactly one full stop distance); this is independent of the capital percentage and measures only whether the trade itself was right or wrong.

### 4.4 Cooldown and counters

- After a close, no new position for `it_cooldown_bars` (default 3) candles;
- 2 consecutive stop-outs → only **1** more entry is allowed that day; another stop → R6's 3-consecutive-loss stop triggers (isomorphic to the XAU "stop after 3 consecutive losses" red line);
- All counters reset on the UTC day boundary (00:00Z); a local-calendar rollover does not count (the taskbook's day-boundary lesson).

### 4.5 Explicit no-trade list (waiting is also a position)

- No structure (range-bound tangle) / convergence (insufficient sep) / hot volatility / spike zone without re-confirmation;
- The opposite trigger just happened 1 bar ago (wait out the cooldown);
- **Do not chase the first leg of a fast rally/sell-off** (the user's own discipline + review #13/#15 evidence: chasing the second-leg top → a ~6-point pullback from the peak; not chasing a high-level cap at 0.75×ATR → the price pinned into a range instead of continuing up);
- Any single item of the 30-second checklist fails;
- ⚠️ **No guessing**: do not chase the first fast rally/sell-off after a data/event shock (**listed ones go through the §4.6 hard event gate**; unlisted temporary shocks are approximated by the volatility "maxTR jump + sep expansion" backstop).

### 4.6 Major-news event gate (red-flag calendar gate; isomorphic to g7/RED_WINDOW)

> Source: XAU taskbook g7 (event-calendar gate) + RED_WINDOW 17:45Z–18:05Z (no new positions) + the precedent of evaluating open positions before an event (docs/gate-trading/gold/joey-Z170I-PRO-GAMING/taskbook/xauusd-agent-taskbook.md:18,58). The engine has no news-calendar feed — this gate uses **preset UTC event windows** to upgrade "discipline around data releases" from a volatility approximation to a deterministic hard gate.

- **Configuration** (instance-level TOML key `news_windows`, **not** a `custom_params`/`--param` key; backtests inject it with `--news`, see docs/backtest.md):

  ```toml
  [[strategy.instances]]
  name = "iron_trader"
  symbol = "BTC_USDT"
  news_windows = [
      { time = "2026-09-16T18:00:00Z" },                              # X/Y default to 15/15
      { time = "2026-10-01T12:30:00Z", close_before_min = 30, resume_after_min = 30 },
  ]
  # time = an ISO UTC string or a bare epoch (< 1e12 is treated as seconds, otherwise ms); empty/absent = gate off
  ```

- **Semantics** (event instant T, flatten X minutes early, resume Y minutes late; judging is always on the candle's **open_time** ms UTC, the same basis as session tiering / the day boundary):
  - **No-entry window = [T−X, T+Y)**: no entry signal is emitted anywhere inside this interval (the breakout-confirmation state m_break_active is invalidated in step — an event spike bar must not serve as the post-resumption confirmation bar); the first tradable candle is the one with `open_time ≥ T+Y`.
  - **Forced flatten = a position is open and its entry candle's open_time < T while the current open_time ≥ T−X**: on the first such bar, synthesise a Flat exit (`exit_reason = news_blackout`, settled on the same basis as the other exits), and keep the no-entry block until the window ends.
  - Multiple windows are unioned (overlapping/out-of-order windows are idempotent); an expired window lapses naturally (a position opened after the event never triggers that window); X=0 means flatten at the close of the event bar itself — leave margin and use X ≥ 1 (at 1m granularity the action lags by ≤ 1 bar).
  - **No runtime toggle**: change the config + restart; the event list is preset (fail-safe: a typo or omission = the gate stays closed; parse time raises strict 5xxx errors).
- **Scope = framework-wide generic**: any UnifiedScalper-family strategy instance can configure the same windows (the base class onKline uniformly intercepts entry signals, while the Flat close-only channel is always allowed — see NewsGate.hpp/UnifiedScalper.hpp); iron_trader is the first wiring of this gate (the flatten semantics rely on its in-strategy state machine).
- ⚠️ Live flatten **execution** (OrderFlowExecutor placing a real closing order for Flat) is still [live enhancement] future work — this gate is fully effective in backtests and on the signal side; the current engine posture = signal_only.

- All gates pass + any trigger → emit `Buy`/`Sell`, with confidence (implementation basis, identical to the C++):
  ```
  confidence = clamp(0.6 + (|sep| / sep_gate_effective − 1.0), 0.0, 1.0)
  sep_gate_effective = it_sep_gate × (session tightening) × (strongest-signal multiplier after 2 consecutive losses)
  ```
  Design rationale: `sep ≥ gate` is the minimum entry threshold ⇒ any qualifying entry has confidence ≥ **0.6** (the engine's default min_confidence), so signals are no longer silently filtered out the way E3 described; 2×gate caps it at 1.0. If a gate fails, nothing is emitted; an exit is **Flat** (the close-only channel, see the §9 mapping), unlike event-driven flip strategies that "never emit Flat" — here Flat is a trading action.
- The exit Flat signal's `indicators`: `{ state, exit_reason, close }`; `exit_reason ∈ { hard_stop, structure_flip, momentum_flip, time_stop, news_blackout (§4.6 event-gate forced flatten) }`.
- `indicators` snapshot (goes to the signal board/report, updated on every close): `{ close, ema_trend, ema_fast, ema_slow, atr14, med_tr20, sep, regime, spike_flag, rsi14, box_high, box_low, state }`;
- **Warm-up discipline** (review #10: a 25-bar short-window warm-up computed a false bearish cross that nearly skewed the direction): `klineNeeded = it_trend_ema + 60` (covering the 20-bar slack for the slope test + the ATR/RSI stabilisation window); before warm-up completes, emit no signal and publish no state — emitting signals from short-window "pseudo-indicators" is forbidden.
- `reason` is human-readable: e.g. `pullback_resume_buy | sep=0.34 | regime=calm`.
- **auto_trade semantics**: `auto_trade=0` only emits signals (signal board/backtest report); `=1` goes through the aggregator and risk-checked order placement (live); signals and the A/B experiment do not interfere (the logging point sits before the auto_trade gate — the M32 contract).

---

## 6. Decision state machine (pseudocode, directly translatable to C++)

```
Every 1m candle close (return immediately before warm-up completes; klineNeeded in §5):
  1. Update indicators: EMA(fast/slow/trend), ATR14, medTR20, RSI14, box (high/low of the last N closed bars); flag spike_flag (spike bar, 4.1 G3)
  2. Update the structural direction struct_dir = bull|bear|none (4.1 G1); determine the current session (main 06:00Z–20:00Z / tight, by open_time UTC)
  3. Update the daily counters (R6: entries opened / consecutive losses / day realised); crossing UTC 00:00Z → reset everything to zero
  4. Event gate (§4.6): any window hit → position open and entry before the event → forced Flat (news_blackout) + settle → return; no position → invalidate the breakout machine, return (no entries)
  5. If position != 0:
       run the 4.3 exit checks; if closed → record PnL (in stop-distance multiples)/reason → cooldown it_cooldown_bars (+ it_tight_cooldown_add in the tight session)
       if still open → return (an open position means management only, R2)
  6. If in cooldown → return
  7. 30-second checklist (R2/R5/R6): any failure → return (record the reason)
  8. Direction gate G2: sep on the same side and above the threshold (×it_tight_gate_mult in the tight session; ×it_streak_gate_mult after 2 consecutive losses)? fail → return
  9. Trigger evaluation (do the 4.2 final re-check first: an opposite event within the last cooldown bars → return):
       T-A pullback: previous close < EMA_fast (long side) and this close > EMA_fast → go long
       T-B breakout: close breaks the box + confirmation rule (+ spike re-confirmation) → enter (return if the RSI hedge filter blocks it)
  10. Entry: R1 check (stop cost ≤ 2%× day-start equity, else return) → record entry, initial stop = entry ∓ it_sl_atr×ATR14 → emit the Buy/Sell signal (with indicators/reason)
```

---

## 7. Parameter table (custom_params channel; both `--param` and TOML work)

| key | Default | Range | Meaning |
|---|---|---|---|
| `it_trend_ema` | 200 | 20–400 | Structure-gate long EMA (1m×200 ≈ a 3.3h horizon) |
| `it_fast_ema` / `it_slow_ema` | 12 / 26 | 2–200 | Momentum EMA pair (the classic MACD pair) |
| `it_atr_len` | 14 | 7–60 | ATR / TR statistics window |
| `it_sep_gate` | 0.25 | 0.05–1.0 | Direction-gate separation floor (ATR-normalised) |
| `it_regime_cap` | 3.0 | 1.5–6.0 | hot test: ATR14 > cap×medTR20 |
| `it_breakout_n` | 20 | 5–100 | Breakout box length in bars |
| `it_rsi_len` / `it_rsi_ob` / `it_rsi_os` | 14 / 70 / 30 | — | RSI hedge filter (T-B only) |
| `it_sl_atr` | 1.5 | 0.5–5.0 | Initial stop distance (×ATR14) = the definition of R |
| `it_be_x_sl` | 1.0 | 0.5–3.0 | Move to break-even when the floating profit reaches N× the stop distance |
| `it_trail_atr` | 1.0 | 0.3–3.0 | Trailing-stop distance after break-even |
| `it_timeout_bars` | 60 | 10–300 | Time-stop cap |
| `it_timeout_x_sl` | 0.5 | 0.1–2.0 | Floating-profit requirement at timeout (× stop distance) |
| `it_cooldown_bars` | 3 | 0–20 | Cooldown after a close |
| `it_tight_gate_mult` | 1.25 | 1.0–3.0 | Direction-gate multiplier in the tight session (20:00Z–06:00Z) (session tiering, G2) |
| `it_tight_cooldown_add` | 1 | 0–5 | Extra cooldown bars in the tight session |
| `it_streak_gate_mult` | 1.25 | 1.0–3.0 | The "strongest signal" multiplier after 2 consecutive losses (1 trade of allowance left that day) |
| `it_max_trades_day` | 6 | 1–30 | R6 daily entry cap |
| `it_equity_ref_usd` | 100.0 | 1–1e6 | Paper starting equity (backtest/paper; ignored live, where the engine equity is read) |
| `it_risk_pct` | 2 | 0.5–10 | R1: per-trade stop-cost cap (% of day-start equity) |
| `it_day_loss_pct` | 5 | 1–20 | R6: daily-loss stop line (% of day-start equity) |
| `it_quanto_est` | 0.0001 | >0 | [approximation] paper-ledger contract multiplier (BTC; other coins must be set from the contract table) |
| `it_fee_est` | 0.0005 | ≥0 | [approximation] paper-ledger taker fee rate; the engine's backtest report net PnL is authoritative |
| `it_consec_loss` | 3 | 1–6 | R6 consecutive-loss stop |
| `it_spike_skip_mult` | 2.0 | 1.0–4.0 | maxTR > N× the SL distance → skip |
| `order_quantity`(atomic) | from config | — | Per-trade order size (R is recomputed from this) |

> The defaults are initial values awaiting calibration by `tools/backtest_sweep.py`; **what changes with the parameters is the frequency and the R size, not the discipline itself** — any parameter set that produces a "high frequency + tiny stop" shape directly violates R1/R6 and is vetoed in review.
>
> `news_windows` (§4.6) is an **instance-level TOML key** (a structured timetable that the double channel cannot hold); it is not in the table above and not part of the `--param` syntax. Backtests inject it with the repeatable `--news TIME[/X[/Y]]` (X/Y default to 15/15).

---

## 8. Acceptance and test checklist

- [ ] Unit test: the three G1 states (bull/bear/no structure) and the EMA-flat test; G2 emits nothing in convergence; G3 hot / spike re-confirmation / huge-TR skip;
- [ ] Unit test: the T-B breakout confirmation rule (break, confirm, re-confirm) and the RSI hedge block; the T-A pullback test;
- [ ] Unit test: the exit chain hard stop → break-even → trailing → structure reversal → momentum reversal → time stop, transition by transition;
- [ ] Unit test: the R6 counters (day-boundary reset / consecutive-loss stop / daily-loss stop / daily trade cap);
- [ ] Backtest acceptance: BTC futures over the recent week (08-30→09-06) with the default parameters — the signal count should be single-digit (a low-frequency feature); more than 30 trades/week → treat the gates as failed, red;
- [ ] Backtest comparison: the E1 control — IronTrader over the same window vs momentum's low gate with 254 trades and −50.25; it should be clearly better (the discipline premium is quantifiable);
- [x] Unit test: the four event-gate cases (2026-09-06): ① the blackout blocks entries ② a position crossing T−X flattens on exactly one bar (news_blackout, with exit_reason/indicators asserted) ③ entries resume after the window ④ the breakout machine idles across the event (on the first bar back it only re-arms, it does not confirm instantly) — all green (1032 total);
- [x] E2E (2026-09-06, BTC futures 08-30→09-06): `--news 2026-08-31T05:46:30Z/10/5` → the baseline 37-minute position (05:28→06:05) was flattened to 05:28→**05:37** (the first bar with open_time ≥ T−X = 05:36:30), while the other 25 trades stayed identical trade by trade; when the event window covers the entry moment → the entry signal is swallowed and there are zero entries inside the blackout window;
- Initial-value measurement (2026-09-06, BTC futures 08-30→09-06, 9,839 bars, qty=20): default parameters **26 trades/week, net −4.74 (win rate 15%, fees 4.10)**; tightening `sep_gate/sl_atr/cooldown/momentum period` individually does **not reduce the frequency** (BTC 1m |sep| is naturally huge, and turnover is set by the number of small pullback events inside the structure window) → the frequency lever is **cooldown/holding gap and time scale**; calibrate parameters via `backtest_sweep.py`; at the maker fee (0.02%) fees halve (−2.28), so favour maker-first live;
- **Sweep calibration (2026-09-06, 480 combinations = 240×2 datasets, data in `results/sweep_iron_{futures,spot}.csv`)**:
  - futures 6.9d: under the taker basis **0 of 240 combinations were profitable** (best −1.18); under maker (0.02%, fees converted linearly + a confirming run) the best was **+1.37** (cd40/sl5/trend400/box60/sep0.3), but **the largest single trade +1.05 = 77% of the net profit** → one lucky trade, not promotable;
  - spot 13.7d: **all 240 combinations lost** under both the taker and the maker basis (gross at best +1.0 over 50 trades);
  - Conclusions: ① **the signal count does not change with cooldown/trend period** (≈26/6.9d): entries come from 3-4 structure windows per day, so the parameters change quality, not frequency; ② sl_atr=5/trend=400 is mildly better (the stop leaves the noise band); the direction is consistent but weak; ③ **BTC 1m pullback-breakout gross is ≈ 0**, so profitability is purely the fee/execution spread → the conclusion is at the execution layer: **this strategy cannot be rescued by parameters on 1m; the marginal lever is the time scale (5m/15m/1h) and maker-first**; the defaults stay at their "initial values" and do not pretend to be calibrated;
- **Full-cycle evidence v1.6 (2026-09-06, BTC futures qty=20; after the data-channel fix 7156f0f, `--interval` really fetches 5m/15m/1h — a non-1m direct REST call does not write back; depth 5m≈35d/15m≈104d/1h≈416d)**:
  - Same week (08-30→09-06): 1m 26 trades **−4.74** / 5m 20 trades −9.06 / 15m 5 trades −4.12;
  - 3 weeks (08-16→09-06): 5m 72 trades **−20.06 (worst)** / 15m defaults 25 trades **−3.57 (gross +0.30 = the first gross-positive result; average win/average loss 1.30)** / 15m calibrated (sl4·sep0.4·to40·ema100·be1.5) 18 trades **+12.93 (the first net-positive result, but 79% comes from a single trade of +10.18)**;
  - **3 months out of sample (06-01→09-06, the key falsification)**: 15m defaults 135 trades −18.51 / 15m calibrated 86 trades −15.22 (still the least bad, with a stable 6.1/week frequency) / 1h defaults 23 trades −7.21 (win rate 43.5%, −0.5/week) / 1h calibrated 8 trades +21.28 (**one huge winner = everything; too few samples to trust**);
  - 48-combination 15m sweep (`results/sweep_iron_trader_BTC_USDT.csv`): sl=4·sep≥0.4·be=1.5 dominates the leaderboard — "stop out of the noise band + only high-quality trades + let winners run", isomorphic to the 1m 480-combination "same direction but weak" result;
  - Winning-trade record (the only big winner in the sl4 set): 08-20 21:30Z Buy 72,705.9 → 08-21 09:00Z close 77,873.0, 46×15m = 11.5h, riding the 08-21 **BTC +9% surge** (QE-lite/CLARITY catalysts); peak floating profit +13.14@79,567 (08:45); the 09:00 reversal bar (79,275→low 77,240) confirmed the exit on its close, giving back to +10.33; a shakeout to −0.3% at 22:00 was survived by the wide R (4×ATR) — that is why it could ride the main upleg; 12 bars after the exit the price never reclaimed 78.5k, so the exit landed in the local top zone;
  - **Conclusion v1.6**: the longer the timeframe, the better the pattern holds (frequency 26→1.6 trades/week, the fee drag disappears from 4.1 to ~1, and the win/loss ratio crosses 1), but **no timeframe/parameter combination on BTC produced a robust out-of-sample positive expectancy** — every apparent profit is a tail trade plus a period-specific move, and it reverts over a 3-month out-of-sample window. **The recommended primary timeframe is 15m** (thickest evidence, 86 statistically treatable trades, parameter regularities consistent across timeframes); 1h is an observation window only (23 trades over 3 months is too thin a sample); 5m is the worst — do not use it. The defaults stay at their "initial values"; a live trial = 15m + the calibrated set + maker-first + small size, with the expectation set at "accumulate a long sample around break-even" — real profitability must wait for a 2-3 month long sample plus a re-test of the tail dependency on the next surge day;
  - Backtest basis: the instance's news_windows is a forward-looking table (from 09-11), so real historical events inside the tested window (e.g. 09-04 NFP 12:30Z) are not in it → **historical backtest windows must add the real events explicitly with `--news`**; adding the 09-04 NFP window produced an identical result bar by bar (the strategy's active period 00–08Z is out of phase with the US macro 12:30Z and it was flat), which is coincidence, not the gate working; gate liveness is shown in the E2E above;
- [ ] Doc backfill: check the ⚠️/pending-fidelity status of each row of the §9 mapping table once the C++ lands.

---

## 9. Material provenance and engine-capability mapping (doc ↔ code ↔ source)

| Clause | Material source | Engine status |
|---|---|---|
| R1 per-trade 2% / R6 daily loss 5% of equity | Iron rules v2 §1/§2/§6 (intraday 5% loss → halt, per-trade 2% review) | Backtest: paper-equity ledger (R0); live: risk gate + maxDailyDrawdown ✅ |
| R3 native stop | Iron rules v2 §3 (futures price_orders); M21 attached SL/TP | [live] ✅; backtest ⚠️ close-price approximation, M33 intra-bar not implemented |
| R4 no averaging down | Iron rules v2 §4; SNDK laddered longs during a decline got trapped (the catching-a-falling-knife counterexample, deepest floating loss beyond 1.956) | Structural guarantee + reduce-only ✅ |
| R5 volatility discipline / regime / spike | Taskbook Gate2; reviews #9/#13; round 446; SNDK spike freeze | Backtestable ✅ (indicators implemented in the C++ phase); spike exclusion = approximation (flag + re-confirmation) |
| Session tiering (main/tight) | XAU taskbook v3.1 session-aware weights (BTC 24/7 → threshold tiering, not a switch) | Backtestable ✅ |
| R6 strongest signal after 2 consecutive losses | XAU "the next trade after 2 consecutive losses must be strongest-signal grade" (16 rounds after FOMC with 0 opens and 0 closes, no FOMO) | Backtestable ✅ |
| Final pre-entry re-check | Round 178 (a decision-to-execution gap incident, net −0.12) | State-machine approximation ✅ |
| Do not chase the first leg / second-leg top | User's own words + reviews #13/#15 | 4.5 checklist ✅ |
| Warm-up blocks short-window false signals | Review #10 (a 25-bar warm-up false-cross instance) | warmup parameter ✅ |
| Immediate handling of unprotected positions | Taskbook step 1 (a naked position = the highest-priority risk item; try a modify lock → close as the fallback) | [live] ✅ / backtest N/A |
| Close-only exits (Flat channel) | Taskbook "no chasing / no flipping"; the limits of the Flip model | BacktestAccount Flat = close without opening (implemented 2026-09-06); live OrderFlowExecutor semantics pending verification |
| R6 daily circuit breaker | Iron rules v2 §6; taskbook red lines (daily loss / 3 consecutive losses / round-trip cap) | Backtestable ✅ |
| R7 break-even and stop trailing | Iron rules v2 §7 | Backtest ⚠️ close-price approximation |
| G1 structure gate (long EMA + slope) | The "follow the big picture" idea of the scalping/resonance family; a lightweight version of the ema-resonance full-permutation approach | New implementation |
| G2 sep ATR normalisation | Signal-board design §4 direction filter + plan C (the engineering conclusion of the confidence ATR-normalisation fix) | New implementation (the key to fixing E3) |
| T-B confirmation rule / RSI hedge / re-confirmation | Reviews #5/#6, M22 rounds 130–132, round 446 | New implementation |
| Time stop / cooldown / day boundary | Taskbook §4 / the day-boundary lesson | Backtestable ✅ |
| Real event-calendar gate (§4.6), maker-first variant, intra-bar SL/TP | Taskbook g7/B2; M16; M33 roadmap; XAU RED_WINDOW | Backtest/signal side ✅ (landed 2026-09-06: preset UTC windows + Flat news_blackout); live flatten execution (OrderFlowExecutor consuming Flat) is still [live enhancement]; the maker-first variant and intra-bar SL/TP await a milestone |

---

## Revision log

| Version | Date | Change |
|---|---|---|
| v1.0-draft | 2026-09-06 | Initial draft: distilled the material (iron rules v2 / XAU taskbook v3.4 / signal board §4 / M22 lesson / M33 evidence) into the IronTrader rule baseline |
| v1.1-draft | 2026-09-06 | Merged the thinking-board mining results (797KB + 204KB of logs + changelog): the confirmation rule's cumulative 13 breakouts — 6 real, 7 false, a 100% separation; the RSI hedge retested on both sides; session tiering; strongest signal after 2 consecutive losses; do not chase the first leg; warm-up discipline; the spike-exclusion convention (flag + re-confirmation approximation) → §0/§3/§4/§5/§6/§7/§9 |
| v1.2-draft | 2026-09-06 | User ruling: the capital gate switches to a **% basis** (the R unit is retired) — R0 paper-equity ledger added; R1 per-trade stop cost ≤ 2%× day-start equity (over the limit the signal is rejected); R6 stop at a daily loss ≥ 5%× day-start equity; break-even/time stop now use "stop-distance multiples" (`it_be_x_sl`/`it_timeout_x_sl`); parameters replaced with `it_equity_ref_usd`/`it_risk_pct`/`it_day_loss_pct` |
| v1.3 | 2026-09-06 | C++ landed (1016 green): registration key `iron_trader`; BacktestAccount gains a **Flat = close without opening** channel; the confidence formula aligned with the implementation (§5); added the `it_quanto_est`/`it_fee_est` [approximation] parameters; §9 gains the Flat-channel mapping; the backtest initial-value table is in §8 (defaults 26 trades/week, −4.74; tightening the gates does not reduce the frequency — a structural BTC 1m conclusion; parameter calibration goes through the sweep) |
| v1.4 | 2026-09-06 | 480-combination sweep calibration recorded (§8 + the two CSVs under results/): no profitable combination under taker; the best maker corner +1.37 but 77% from a single trade; spot all negative; 1m gross ≈ 0 → the defaults stay at their initial values, and the marginal lever is the time scale and maker-first |
| v1.5 | 2026-09-06 | **The major-news event gate landed** (§4.6, 1032 green + an E2E record): instance-level `news_windows` with preset UTC windows (backtest `--news`), no-entry [T−X, T+Y) + forced Flat (`news_blackout`) for positions crossing T−X, framework-generic (NewsGate.hpp pure logic + UnifiedScalper base-class backstop), with iron_trader as the first wiring; **basis correction statement**: session tiering / day boundary / this gate are all judged on the candle's **open_time** UTC (earlier §4.1/§6 wording said close_time, inconsistent with the implementation; the doc and the implementation are now unified on open_time) |
| v1.6 | 2026-09-06 | **Full-cycle evidence recorded** (§8; the data-channel fix 7156f0f made `--interval` work for 5m/15m/1h): same-week / 3-week / 3-month out-of-sample comparison + a 48-combination 15m sweep — the longer the timeframe, the better the pattern holds (15m is the first gross-positive, win/loss ratio 1.30), but **no timeframe shows a robust out-of-sample positive expectancy**; every apparent profit is a tail trade. **The recommended primary timeframe is 15m** (86 statistically treatable trades; the parameter regularity sl4·sep0.4·be1.5 is consistent across timeframes); 1h is an observation window and 5m is the worst — do not use it. The defaults stay at their initial values |
