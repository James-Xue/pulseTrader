# ETH Futures Sub-Agent — Taskbook v1.1 (Takeover Edition)

> Read this file at the start of every session; update it when done. Task status: ⬜ todo / 🔄 in progress / ✅ done
> Rule details are governed by `strategy/eth-subagent-strategy.md`; ask the user first when a rule is ambiguous

## ⚠️ Takeover notice (2026-08-20 00:50 Beijing)

- **The ETH grid is already running live** (deployed on the joey machine before it lost network; this machine took over its management). **Do not create a new grid — take over the existing one!**
- Exchange-side state (direct REST probe at 00:44):
  - Position: **ETH_USDT -404 contracts @2046.14 (floating loss -170.5)** — this includes the user's manual position; the grid agent's share covers only the filled grid levels within it
  - 14 open orders: 13 reduce-only buy orders under `t-eth-grid-tp-*` (2010~2070, 2 contracts each), 2 short orders `t-eth-grid-2110/2115` (2 contracts each), and **the user's App order `app` -100 @2128 (do not touch!)**
  - Futures account: total 1192.8 / available 801.3 USDT; CFD 50 USD; spot 0.06 USDT
  - Market: mark ~2097; the current price is above the grid's filled segment and below the remaining short levels (2110/2115)
- **User authorisation for tonight (00:42)**: the ETH sub-agent is authorised to **watch the market all night + trade for real + update its own strategy and dynamic grid range at any time**.
- **Night autonomous mode (added 00:53, until 08:00 Beijing)**: the user is asleep; **do not ask the user questions or wait for user decisions — decide every action autonomously and write it to disk** (thinking board / review / state file). The user will read the files after 08:00. When protection line A/B triggers, follow the night-autonomous version of §4 (do not alert and wait — act directly).
- **Major event**: the user flagged a **major US data release around 02:00 tonight (Beijing)** (FOMC meeting minutes, red-flagged on the calendar; the calendar also lists 00:00Z, which differs — take the user's 02:00 Beijing = 18:00Z as authoritative). **Event window 01:30–02:30 Beijing (17:30–18:30Z)**: no re-anchoring, no placing new grid levels, no cancelling TPs; only TP/protection-line management and doubled checks; resume normal operation after 02:30.
- **Tooling**: all exchange-side operations go through `tools/eth_ledger.py` (orders/positions/account/klines/book/place/tp/cancel, direct signed REST); MCP is read-only (status/account/positions/get_orders). **Do not change the engine config or restart it tonight** (the gold agent is using it).
- Red lines (not changeable autonomously): **short only / 2 contracts per level / no per-level stop-loss / stop trading at a daily loss of -10 USD / protection line B (grid floating loss ≤ -30 USD) alerts and the user decides / never touch the `app` prefix or the user's position share / place_trigger_order strictly forbidden / missing reduce_only strictly forbidden**.

## ⚠️ Grid placement suspended (2026-08-20 04:56, parent-session instruction; until the user says to resume)

- At 04:51 the price spiked (2124→2217, +117 points): the user bought 486 contracts at market @2217.52 and **closed everything** (including the grid's 2110/2115 fills and the three app orders); all orders were cleared. Now 0 positions and 0 open orders.
- **The agent only monitors; placing a new grid / re-anchoring / placing orders is forbidden** (state.hold=true). Resuming requires a user instruction or a relay from the parent session. Monitoring duties continue as before: check the account/positions/open orders every round, protect the user from abnormal states, and write anomalies to disk.
- If the user places orders/opens positions in the App again: observe only, do not interfere, do not copy the trade.

## Current tasks

1. **🔄 Takeover reconciliation** (every round): compare `eth_ledger.py orders` against positions to maintain grid integrity:
   - The 13 TP buy orders (2010~2070) are listed and carry reduce_only=true; the 2110/2115 short orders are listed
   - The user's `app` orders are watched but never touched; no order outside `t-eth-grid-*` is touched
2. **🔄 Re-place in the loop**: when a TP buy order fills (the order disappears and the price touched ≤ the TP price) → if the current price is below the original level price, re-place the same-level short order plus a new TP (classic grid)
3. **🔄 Fill in missing levels**: re-place a filled grid level after its TP is realised; if the 2110/2115 levels fill (the price breaks up through them) → immediately place a TP (fill price - 2×step); **whether to extend new levels downwards is decided autonomously by the agent per strategy §3.2/§5** (authorisation to update the grid range autonomously)
4. **🔄 Re-anchor check**: per strategy §3.1 (mid < lower bound - 3×step → follow down; mid > upper bound + step → do not auto re-anchor, hand over to protection line A; 30-minute cooldown)
5. **🔄 Protection lines (night-autonomous edition)**: A: 15m close > grid top + 4×step (current grid top 2115 → 15m close > 2135) → **act autonomously**: close the grid's own share with a reduce_only market buy (computed as the number of filled grid levels recorded in the state file × 2 contracts; never exceed that share) → write the record to disk → re-anchor and place levels in the new range per §3.1; B: grid floating loss ≤ -30 USD → likewise close the grid share autonomously + write to disk + re-anchor. **Neither line waits for a human any more.**
6. **🔄 Event window**: 01:30–02:30 Beijing = special rules (see the takeover notice); before the event, record the price/volatility state on the thinking board every round; after the event, act autonomously per scenario: a price crash → TPs realise, so re-place in the loop; a price surge through the grid → protection line A, autonomous edition.
7. **🔄 Self-update of strategy/grid range** (authorised): step/anchor/TP distance/number of levels/cooldown may be adjusted autonomously within the red lines; every adjustment must: ① be written into the strategy document's change log ② have its rationale recorded on the thinking board ③ be synced to the state file
8. **🔄 Night survival rules**: write the full context to disk every round (the state file holds the filled grid level count, position share, anchor, cooldown deadline); do not rely on the previous round's memory — re-derive everything from the exchange side plus the state file each round; on an API error, skip the round; when unsure, do nothing
9. **⬜ Multi-agent parallel verification**: do not interfere with the gold / SanDisk / Unitree agents (the engine is shared read-only; ETH orders go over a direct connection)

## Parameter baseline (decided in v2 on 2026-08-21; red lines are immutable, the agent may adjust everything else autonomously and write it to disk)

| Parameter | Value |
|---|---|
| Direction | Short only (red line) |
| step | **ATR-adaptive clamp(round(0.5×ATR15m),3,8)** (landed in v2; write state.grid.step to disk on every update, with tp_distance=2×step synced) |
| TP | fill price - 2×step |
| Contracts per level | 2 (red line) |
| **Trend gate (v2)** | **Place new levels / re-anchor only under a bearish 15m EMA alignment; under a bullish alignment only manage the existing book (TP/protection lines), with state recording trend_gate=bullish** |
| **Spike freeze (v2)** | **If state.grid.spike is set (1m gain > max(1%, 3×ATR15m), detected and written by the watchdog) → freeze new levels for 30 minutes** |
| Protection line A (v2) | **15m close or 1m close > top + 2×step** (no longer waits for a human: autonomously close the grid share + write to disk + re-anchor per §3.1) |
| Protection line B | Grid floating loss ≤ -30 USD → same as A |
| Daily loss stop | Realised loss ≤ -10 USD (red line), **resets at 08:00 Beijing time** (v2 ruling) |
| Re-anchor cooldown | 30 minutes |
| Signal source | eth_scalper_ETH_USDT from get_signals (EMA bearish cross + spike filter) may serve as a supplementary reference for the trend gate |

## Completed

- **2026-08-20 07:43: task ended (the user ordered the sub-agent shut down)** — the relay loop / watchdog / hourly inspection / log monitoring all stopped; state.mode=stopped, with the hold and daily-loss-stop records retained; resuming requires an explicit user instruction. Snapshot at shutdown: the ETH -210 position and the `app` orders were all live user App trades; the agent did not intervene. 51 LLM rounds plus the watchdog fallback in total, with the grid's realised loss frozen at -38.6. Morning report: `review/eth-morning-report-20260820.md`.
- 2026-08-20 04:52: **round22 major event** — the price surged 2128→2222 through the grid during 04:50-04:51; the user's App auto order (ao-) at 04:51:06 bought 486 @2217.52 in one fill and closed everything (grid 24 + user 462), leaving the grid at zero, 0 open orders, and the account total going 1184.59→477.85; the grid share's realised loss ≈ -39.2 → **daily-loss stop (-38.6 ≤ -10 red line): no new positions or re-placements for the rest of the day**. Consistent with the watchdog's verdict (hold/flattened already set). Review: `review/eth-review-20260820-045200.md`.
- 2026-08-20 00:50: **takeover-edition taskbook v1.1** — measured exchange-side state + user authorisation (all-night watching / real trading / autonomous updates) + the FOMC event window
- 2026-08-20 00:44: **account/position/order probe** (futures total 1192.8 / avail 801.3; -404 @2046, floating loss -170.5; 14 open orders)
- 2026-08-20 00:40: strategy document v1 created (mirroring SNDK v2); `tools/eth_ledger.py` toolbox
- 2026-08-20 00:22: directory skeleton created (SNDK session)

## Conventions

- Grid orders: `eth-grid-<price>`; TP buy orders: `eth-grid-tp-<fill price>`; other agent orders: the `eth-agent-` prefix
- Cancellation: only orders under our own prefixes `eth-grid-*` / `eth-grid-tp-*` may be cancelled; the user's `app` prefix is absolutely off-limits
- State/review: `state/eth-agent-state.json` / `review/eth-review-*.md`; write a review for every close
- Conservative first: **when unsure, do nothing**; on an API error skip the round and never retry an order in an error state
- ⚠️ **The parent session's mechanical watchdog (`tools/eth_watch.py`, resident in the background, one round per 150s) is the baseline protection process: killing/stopping/modifying/editing it is forbidden** (it handles TP-integrity re-placement and automatic protection-line handling, complementing the agent's duties). If it reports an error, only record it — do not intervene; the parent session fixes it. Likewise, terminating the relay loop (the while loop in `tools/eth_relay.sh`) is forbidden.
