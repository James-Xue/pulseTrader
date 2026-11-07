# BTC Strategy-Weight A/B Paper-Trading Validation Experiment (M32)

> Status: **pilot not started** (waiting for M32 signal logging to be deployed on the VPS and accumulate data)
> Start date: the day the first record lands in the engine's `signals` table
> This document = the experiment contract. §7 decision rules are fixed up front; once the experiment ends they may only be executed, not changed after the fact.

## 0. Goal and hypotheses

The question to answer: **is AI daily weight tuning (arm B) significantly better than the equal-weight baseline (arm A) in net-of-fee returns?**

- H0: the AI-weighted portfolio ≤ the equal-weight portfolio (no difference or worse net PnL)
- H1: the AI-weighted portfolio > the equal-weight portfolio

Comparison arms:

| Arm | Weighting strategy | Purpose |
|---|---|---|
| A equal-weight | all 1.0 (current engine default) | baseline |
| B AI-tuned | AI proposal daily at 00:30 (Asia/Shanghai) | the subject under test |
| C AI risk control | equal-weight + AI 5-minute patrol records advisory only (not executed) | reserve for the next experiment round, not enabled this round |

**The paired structure is the core**: A/B replay on the same signal log, the same tape (ticker_ticks) and the same fill model → the daily difference cancels out market-segment differences, so even a small sample has resolving power.

## 1. Data foundation

- `signals` table (new engine-side table in M32, append-only): every raw strategy signal
  (ts_ns/strategy_id/symbol/type/confidence/price/reason/indicators) +
  the aggregator consensus row (strategy_id = `signal_aggregator`, excluded on replay and kept for later
  live-fidelity checks). **All strategy signals are persisted before the auto_trade gate** → the log is
  independent of the weighting strategy, which is the precondition for counterfactual replay.
- Price path: `ticker_ticks` (persisted tick by tick when record_market=true; futures
  klines are not recorded as of M30, and the tape is sufficient).
- Real-fill calibration: `trades` table (trades.db). Initial values (2026-09-06, 409 rows on this machine):
  SNDK futures maker orders 144 / fill rate 72.9% / maker-leg slippage 0bps /
  market leg 0.02–0.27bps.

## 2. Replay semantics (tools/trial/paper_engine.py, mirrors the engine)

- **Aggregator** (a line-by-line replica of SignalAggregator.cpp): flat ignored; per-symbol accumulate
  buy/sell weighted confidence and weight_sum; normalized = dominant/ws < threshold (0.6)
  → no emission; **the cooldown (30s) gate comes before emission, and when gated the accumulation is not reset but keeps growing**;
  cleared after emission; default weight 1.0. Note the real engine property: **residual opposite-side stacking can gate emission shut for a long time**
  (mixed-flow normalization < 0.6); a direction reversal needs the new direction to be ≥ ~1.5× the old residual — that is
  engine semantics, not a bug.
- **Ledger** (PaperTrader state machine): entry is a maker-first limit order placed at the signal price, filled by the tape
  re-touching it (buy: last ≤ P); timeout (5s) → taker at the prevailing last as fallback; during the resting period an
  opposite consensus → cancel and abandon; during the holding period evaluate tick by tick: TP ladder 0.5%/1%/2% × 1/3
  (taker), Trailing SL (0.5% from best, taker), max_hold 300s (taker),
  opposite-consensus close (taker); same-direction consensus does not scale in; the entry fee is allocated to each leg in proportion to the closing volume.
- Fee defaults: maker 0.02% / taker 0.05% (Gate perpetual tier, measured 2026-08;
  the CLI can override).
- Single position, no scale-in, exit price = tape last (taker leg). **Paper ≠ live absolute
  return**: the fill model has error, but A/B uses the same rules on the same tape, so the error weighting is irrelevant and the paired
  difference cancels it to first order.

## 3. Pilot week (week 1, signal-rate reconnaissance)

- Record: daily signal count per strategy, aggregator emissions, A/B round-trip counts, signals-table growth
- Power: ≥ 300 round trips per arm (win-rate standard error ≈ 2.9%, resolving a ~5–6% win-rate difference)
- If the emission rate is ~20–60/day → the main experiment runs 14 trading days with an interim checkpoint on day 7
- **If the emission rate is too low (< 5/day)**: stop and re-evaluate (with sparse signals, first enlarge the strategy set
  or adjust the threshold to get statistical meaning — this decision is made within the pilot week, so it does not count as changing the rules mid-course)

## 4. AI Oracle specification (once per day)

- Trigger at 00:30 Asia/Shanghai; only look at rounds **already closed** before 23:59 the previous day
- Input (numeric rows): per-strategy 24h/72h round-trip count/net PnL/win rate/fee share/mean slippage +
  regime features (1h/24h realized volatility, EMA fast/slow ratio, ATR%, spread bps, funding rate)
  + current weights + the last 5 self-tuning records
- Action space (enforced by the harness, the AI cannot exceed it): output
  `{"weights": {strategy_id: w, ...}, "rationale": "..."}`;
  constraints **w ∈ [0.05, 0.5], single change ≤ ±0.1, sum within the group = 1**
- Audit: every decision is written to the `decisions` table (old→new weights, input summary hash, model,
  prompt version, raw response), temperature 0, model and prompt versions fixed

**5-minute patrol**: records advisory only; does not execute and does not enter this experiment's statistics.

## 5. Experimental discipline (anti-confounding, fixed up front)

1. Parameter freeze: set_strategy_param is forbidden during the experiment (AI parameter tuning is a separate variable)
2. Roster freeze: BTC strategies are neither added nor removed (new strategy ideas are backtested on the log first and not admitted)
3. Downtime handling: engine restarts/days with recording gaps (>10% of 1m windows missing ticks) are excluded from the paired statistics
   but kept in the report; schedule the systemd kill-9 self-heal test outside the experiment window
4. Never lose a strategy: the log records before aggregation, and pressing a weight down to the 0.05 floor does not stop it from
   emitting signals → every strategy can be reviewed end to end, a structural escape hatch against overfitting

## 6. Metrics

| Dimension | Metric |
|---|---|
| Primary criterion | mean and sign of the A/B daily **paired net PnL difference** (paired t + sign test) |
| Secondary criteria | expectation per round trip (net), max drawdown per arm, round-trip count, fee/net-profit ratio |
| Stability | day-to-day weight oscillation (large daily changes = penalty; live trading would be equally jittery) |
| Sensitivity | fee upper/lower bounds (all-maker vs maker+taker) run twice; conclusions must agree |

## 7. Decision rules (fixed before the experiment)

After the 14 trading days of the main experiment:
- **GO (AI weights take over live trading)**: paired daily difference mean > 0 **and** ≥ 10 of the 14 days positive
  **and** the AI arm's total net profit > 0 — all three conditions required
- **Defer**: GO not met but the AI arm is not clearly worse → extend by 14 days (once only); still not met
  → NO-GO
- **NO-GO**: the AI arm is worse or break-even → live trading stays equal-weight and arm A continues; AI is downgraded to pure risk-control
  advice (i.e. the arm C role)
- Regardless of the result: the three arms' equity curves + weight trajectories + all AI decision records are archived
  (data/trial/, not committed to git, archived by a human decision)

## 8. Tools and data locations

| Item | Location |
|---|---|
| Engine signal log | VPS `~/pulseTrader/data/trades.db` `signals` table (M32) |
| Replay core | `tools/trial/paper_engine.py` (+ `tests/test_paper_engine.py`, 24 cases) |
| Replay CLI | `tools/trial/run_replay.py` — equal/ai arms |
| Calibration | `tools/trial/calibrate_fill_model.py` |
| Artifacts | `data/trial/<symbol>/<arm>/{round_trips.csv, daily_summary.json}` |
| Engine implementation | `src/trade_recorder/SignalRecorder.{hpp,cpp}` (config: `sqlite.record_signals`) |

Run examples:
```bash
# equal arm (equal weight 1.0)
python3 tools/trial/run_replay.py --db data/trades.db --symbol BTC_USDT --arm equal
# AI arm (weight file normalized)
python3 tools/trial/run_replay.py --db data/trades.db --arm ai --weights-file w.json
# calibration
python3 tools/trial/calibrate_fill_model.py --db data/trades.db
```

## 9. Implementation status

- [x] M32 signal-log engine implementation + unit tests (5 cases), commit 1b4028d
- [x] Replay toolchain + 24 cases + initial calibration values, commit a1dd5ac
- [ ] Deploy M32 on the VPS (signal recording starts the clock) — in progress
- [ ] Pilot week → main experiment → §7 ruling
