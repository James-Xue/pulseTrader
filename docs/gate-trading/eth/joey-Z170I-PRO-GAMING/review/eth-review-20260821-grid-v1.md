# ETH short-grid v1 full-cycle review — 2026-08-21 (Beijing time)

> Material: trading records (633-line thinking board + status-file notes + two retro write-ups already written) + live snapshots (08-21 engine/exchange).
> Conclusion first: **the mechanical layer ran entirely as designed; the strategy layer was derailed by "three spikes through the top" and by the user's position being swept up with it — ETH was in a one-directional uptrend at the time, and a short grid is catching a falling knife. What was missing was a trend gate, not execution.**

## 1. Full Accounting

| Item | Value |
|---|---|
| Lifecycle | 08-19 23:16 deployed → 08-20 04:51 fully closed → 07:43 shut down |
| Grid TP realized | First night 10 fills +5.0 (fill_price basis) + overnight 3 fills +0.60 ≈ **+5.6 USD** |
| Grid share loss | 24 contracts @2020~2115 closed together at 2217.52 by the user's single 486-lot App buy ≈ **-39.2 USD** |
| Grid net result | ≈ **-33.6 USD** (plus ≈ -668 from the user's own position loss, which was the user's own decision and not part of the grid) |
| Spike count | 23:21 +9% (1905→2118), 00:01 +1% (2082→2102), 04:50 +4.4% (2124→2222) → **3 sharp rallies through the top within 4 hours** |

## 2. Timeline and Causal Chain

```
23:16 Deployed 20 grid levels (2020~2115, step 5, 2 contracts per level, first anchor 2020)
23:21 Spike #1: all 20 levels filled within 5 minutes → 19 TPs placed → 10 TPs filled +5.0 → re-place loop begins
23:47 After TP-2085 filled, re-placement rejected = 9103 notional gate breached (engine futures budget 6000 did not count ETH grid notional)
00:01 Spike #2: four levels 2090/2095/2100/2105 filled, TPs could not be placed → four naked levels (loop frozen, unresolved)
00:05 Network dropped → joey machine wrapped up; MECHREVO machine took over at 00:50, strategy v1 (mirrors SNDK v2)
01:30-02:30 FOMC event window (rules executed correctly, zero violations)
02:04 3 TPs filled +0.60
04:13 2110/2115 filled → full 12/12
04:50 Spike #3: 2124→2222 (+98 points in 2 minutes), protection line A (15m close > 2135) had no time to react
04:51:06 User's App auto order buys 486@2217.52 and closes everything (grid 24 + user 462) → grid back to zero
04:52 Daily-loss stop (-38.6 ≤ -10) took effect → hold mode until shutdown at 07:43
```

## 3. Key Findings (supplemented by today's 08-21 live snapshot)

1. **The engine view retained 15 ETH ghost positions** (open_time 08-19 23:23~00:04, strategy_id=eth-grid-*, quantity=2, exchange_position_id=**empty**): the exchange side had already been flat since 04:51, but sync_positions matches on exchange_position_id, **and ghost positions with an empty id can never be pruned**; each also carries a fake upnl of +40~42 (computed with price=0), together inflating unrealized_pnl by **+623 of fictitious value**. → The sync pruning logic needs fixing, or restart the engine to clear the view.
2. **ETH price ~2353** (08-21): +238 points above the old grid's upper bound of 2115. If v1 ran again it would still be wiped out — ETH was in a one-directional uptrend from 08-19 to 08-21 (1905→2353, +23%), **so entering a short grid at any point in that window is counter-trend**.
3. ETH's 3 strategies (momentum/mean_reversion/supertrend) were **paused** in the engine and produced no ETH signals; eth_scalper (M26) was not enabled.
4. The user's App had re-entered yesterday morning (-210@2251.91); the position list no longer shows any user ETH position (already exited).

## 4. Lessons (Input for the Upgrade)

### What went right (keep)
- TP re-placement loop, event-window discipline, protection-line accounting, daily-loss stop, zero disturbance to the user — the mechanical layer had zero mistakes throughout
- TP realized PnL was accounted by fill_price (not the order price); correcting the naive +2.2 to +5.0 was the right direction

### What went wrong / boundary cases (need fixing)
1. **No trend gate (fatal)**: the grid only places shorts above the current price, which in an uptrend means catching a falling knife. There should be gating: during a bullish trend/spike, place no new levels and only manage existing inventory.
2. **Protection line A has no reaction time against a seconds-scale spike**: a 15m close trigger plus limit orders was completely pierced by +98 points in 2 minutes. A faster detection dimension is needed (1m price change / sudden ATR shift), or accept this as an inherent cost.
3. **The 9103 notional gate was misconfigured**: the futures budget of 6000 was calculated as "SNDK + headroom" and did not include the ETH grid notional (~5247+); reduce-only buy orders were also counted as new notional rather than offset → the loop froze and four levels ran naked. This is an engine configuration-layer issue and can be fixed.
4. **Position merging side effect**: under futures one-way position mode, the grid share and the user position merge, so a user market order clears the grid along with it — a structural problem; the grid cannot operate independently of user actions. Mitigation: sub-account isolation, or run the grid only in time windows/on coins where the user is inactive.
5. **Ghost position cleanup**: sync is powerless against positions with an empty exchange_position_id → inflated PnL and misleading judgement.
6. **Daily-loss basis undecided** (UTC day reset at 08:00 vs Beijing day reset at 00:00); fixed step of 5 vs ATR-adaptive step (step=3 was better when the late-night ATR was 3.44) also undecided.

## 5. Upgrade Proposals (for decision)

1. **Trend gate**: precondition for placing grid levels = 15m EMA bearish alignment (or an EthScalper bearish signal); in bullish alignment, place no new levels and let existing inventory run only TP/protection lines;
2. **Spike detection**: 1m price change > k×ATR15m or > a fixed % (e.g. 1%) → freeze new grid orders for X minutes, existing TPs continue as normal;
3. **Protection line A moved forward**: lower the threshold from top + 4×step to top + 2×step, and also trigger on a 1m close beyond the line;
4. **9103 fix**: recompute the trading.toml futures budget as "SNDK + ETH peak notional + headroom" (6000→12000+, maxSymbolNotional 5500→6500), with reduce-only not counted as notional (engine side);
5. **Ghost positions**: add an "empty exchange_position_id and no matching position on the exchange → prune" branch to sync pruning;
6. **Daily-loss basis**: decide on a Beijing-time 08:00 reset (consistent with the overnight task document).
