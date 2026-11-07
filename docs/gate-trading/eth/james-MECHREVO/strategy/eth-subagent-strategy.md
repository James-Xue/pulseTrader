# ETH Ethereum Perpetual Auto-Trading Sub-Agent — Strategy Document v2

> **v2** (2026-08-21, upgraded after reviewing eth-review-20260821-grid-v1.md) · Runs on the pulseTrader engine (mainnet live) · This file is the rule baseline for the ETH sub-agent; review it before making changes
> v2 highlights: **trend gate + spike detection + protection line A moved forward** — the fatal flaw of short-chasing grid v1 was "catching a falling knife in an uptrend + no reaction time when a second-scale spike punched through the top" (3 rapid squeezes in 4 hours; at 04:51 the user's App close also closed it at -39.2)
> The structure mirrors `sndk/joey-Z170I-PRO-GAMING/strategy/sndk-subagent-strategy.md` **v2** item by item (dynamic rolling anchor + ATR step + whole-grid protection line), with price/notional/fee replaced by measured ETH_USDT values — **do not copy the risk-management figures from gold, and do not blindly copy SNDK's absolute values; this document is authoritative in all cases**

## 1. Overview

Run an always-on background sub-agent on the pulseTrader engine that executes **short-chasing grid v1** (= SNDK v2 structure) on ETH_USDT (Gate.io perpetual):

- **Short only** (mirrors the SNDK authorization scope; no longs, no counter-trend bottom-fishing)
- **Trend gate (v2)**: place new levels / re-anchor only when the 15m EMA is in bearish alignment (EMA_fast < EMA_slow); during bullish alignment **manage existing positions only** (take TP fills, protection line) and place no new levels (§3.1c)
- **Dynamic rolling anchor**: the grid zone is not fixed and follows the market. Anchor A = round(order-book mid + 1×step, step), grid zone = [A, A + 12×step]; when price moves away from the grid, re-place the whole grid (§3.1)
- **ATR-adaptive step**: step = clamp(round(0.5 × ATR15m), 3, 8); 12 levels (initial zone width 36 points). step/tp_distance/grid_top are persisted to the state file on every update, and the mechanical watchdog reads them from state (eth_watch.py v2)
- **2 limit short orders per level** (1 contract = 0.01 ETH ≈ 21 USD notional @2097; 2 contracts ≈ 42 USD per level, total grid notional ≈ 500 USD)
- **Take-profit attached to every level**: immediately after a fill, place a reduce-only limit buy (reduce-only), with price = that level's fill price - 2×step (-6 when step=3). WARNING: Gate price_orders trigger orders do not support partial closes (verified on 1017/1014/1021/1038), so **place_trigger_order is strictly forbidden**; TP always goes through `open_order` as a reduce-only limit buy
- **No per-level stop-loss** (mirrors the SNDK user requirement); **whole-grid protection line** (§4)
- **Cyclic re-placement**: when a level's TP fills and the current price is below that level, automatically re-place a short order + a new TP at the same price (classic grid)
- A round takes about 25-30 s: read the live state → reconcile orders → top up/re-place → check fills → place TPs → re-anchor check → protection-line check → risk check
- Write a post-mortem md for every closing trade to `0_note/gate-trading/eth/james-MECHREVO/review/` (`eth-review-YYYYMMDD-HHMMSS.md`)

## 2. Market and cost background

| Item | Value |
|---|---|
| Trading direction | ETH_USDT perpetual (`market_type=futures`; since M22 manual orders can also be executed under the CFD active direction) |
| Contract spec | 1 contract = 0.01 ETH (quanto 0.01; the risk gate uses true notional) — the same quanto as SNDK, per-contract notional ≈ 21 USD |
| Market data | 1m klines + ticker + order book (all futures channels) |
| Fees | Perpetual taker 0.075% / **maker -0.01% (rebate)** — the limit grid is maker throughout and nets a rebate (better than SNDK) |
| Funding | -0.00013/8h (measured 2026-08-20) — **shorts receive funding**, which favors a short-only grid |
| Leverage | Perpetuals allow up to 200x; the grid uses low leverage (≤10) or the default and relies on 12 levels for diversification rather than leverage |
| Snapshot (08-20 00:40) | mark 2097.6 / last 2094.97; ATR15m (mean 1m TR over 40 bars) ≈ 2.87, ATR30m ≈ 3.34; order_size_min=0 (no minimum-quantity problem for 2 contracts); price precision 0.01 |
| User position | WARNING: **the user's own ETH_USDT short (managed in the App, 10 contracts @1912 as observed on 08-19) — observe only, never touch** |
| Engine strategies | ETH's 3 strategies (momentum/mean_reversion/supertrend, signal_only) are **pending configuration** (see the taskbook), mirroring the SNDK approach |

## 3. Grid operation protocol (core)

**Per-round checklist** (`list_futures_orders ETH_USDT` = every exchange-side order; `list_trigger_orders` = trigger orders; judge prices with the order-book mid = (best_bid+best_ask)/2 — WARNING: get_market's ticker.last has a cache-lag bug, do not use it for price decisions):

1. **Top up missing levels**: if one of the 12 levels has no order (not re-placed after a fill / cancelled) and mid has not yet risen above that level → re-place an `eth-grid-<price>` limit short of 2 contracts
2. **Fill → place TP**: when a level fills (t-eth-grid-* disappears from list_futures_orders and the current price has been ≥ that level) → immediately `open_order`: symbol=ETH_USDT, side=buy, quantity=2, market_type=futures, type=limit, price=fill price-2×step, **reduce_only=true**, client_order_id=eth-grid-tp-<fill price>. WARNING: place_trigger_order is strictly forbidden; omitting reduce_only is strictly forbidden (that would open a reverse position)
3. **TP filled → re-place**: the exchange-side t-eth-grid-tp-<fill price> order disappears (filled) and mid is below that level → re-place a short order at the same level price (cycle)
4. **Re-anchor check** (§3.1, every round)
5. **Whole-grid protection-line check** (§4, every round)
6. **Do not interfere**: the user's App orders and manual ETH position — look, do not touch

### 3.1 Re-anchor protocol (dynamic rolling anchor)

- Anchor A = round(mid + 1×step, step), grid zone = [A, A + 12×step], short only → the zone is always above the current price waiting for a bounce
- **Re-anchor triggers (any one)**:
  - a) mid < grid lower edge A - 3×step: price has moved far below the grid (the downtrend continues and the grid sits idle) → move the anchor down to follow
  - b) mid > grid upper edge + 1×step: price has broken up through the grid → **do not re-anchor automatically**; switch to the protection line A check (unrealized loss on filled levels is handled by the protection line)
  - **c) trend gate (v2, precondition)**: after any a/b trigger, **check the trend before acting** — re-anchoring/placing new levels is allowed only when EMA_fast < EMA_slow (bearish alignment); under bullish alignment → manage existing positions only (TP fills / protection line), **no new levels and no re-anchor**, record `trend_gate = "bullish"` in the state file and wait for a reversal. Lesson from short-chasing grid v1: ETH rose +23% in a single direction from 08-19 to 21 and the grid shorted into it the whole way. **Quick read**: the `eth_scalper_ETH_USDT` entry from get_signals always carries `trend_state` (bullish/bearish/neutral) + `spike` (0/1) in its indicators; when freshness is ≤120 s, use it directly as the gate instead of computing EMA yourself
  - **d) spike freeze (v2)**: when state.grid.spike exists (the mechanical watchdog detects a 1m move > max(1%, 3×ATR15m) and persists it) → freeze new level placement/re-anchoring for 30 minutes; existing TPs are managed as usual; resume once the spike clears
- **Cooldown**: ≥ 30 minutes since the last re-anchor (anti-chatter, avoids repeated cancel/replace in a choppy market)
- **Action**: first cancel all of your own `eth-grid-*` short orders and any leftover `eth-grid-tp-*` orders (your own prefix only) → confirm everything is cancelled → place 12 levels from the lower edge upward under the new anchor (each via `open_order`: sell, 2, futures, limit, price=<level price>, client_order_id=eth-grid-<price>)
- Persist state after re-anchoring (step/new anchor/cooldown), and record the old and new anchors plus the trigger reason on the thinking board

## 4. Exit rules

> ETH futures have **no exchange-native attached SL/TP** (open_order's sl_price/tp_price are CFD only); take-profit = a **reduce-only limit buy** per level (resting exchange-side, effective even if the engine crashes; when touched it closes exactly that level's 2 contracts). price_orders trigger orders only support **full close** and are disabled for the grid. **No per-level stop-loss** (mirrors the SNDK user scope).

| Rule | Trigger | Action |
|---|---|---|
| Per-level take-profit | price ≤ that level's fill price - 2×step | the reduce-only limit buy automatically closes exactly that level's 2 contracts |
| **Whole-grid protection line A (v2)** | **15m close or 1m close** > grid top + **2×step** (v1 used 4×step — the 04:50 spike ran 2124→2222 in only 2 minutes, leaving no reaction time for a 4×step limit judgment) | reduce-only market buy to close the grid's own share (the state file's recorded filled level count × 2; exceeding that share is strictly forbidden) → write the post-mortem → decide per §3.1 (including the trend gate/spike freeze) whether to re-anchor and place levels. The mechanical watchdog and the LLM agent follow the same rules |
| **Whole-grid protection line B** | cumulative grid unrealized loss ≤ **-30 USD** | same autonomous handling as protection line A (close the grid share + persist + re-anchor) |
| Daily-loss stop | cumulative realized loss for the day ≤ **-10 USD** | no new positions/re-placements for the rest of the day (the existing grid is managed as-is); **scope (decided in v2): reset at 08:00 Beijing time** (consistent with the overnight taskbook; the 00:00 night session does not start a new day) |

> Grid share scope: count only the filled portion of `eth-grid-*` orders (2 contracts per level), unrelated to the user's manual position; close with a reduce-only market buy, quantity = number of filled grid levels × 2, never over-close.

## 5. Risk and operation protocol

- Confirm every round: `trading_halted=false`, rate-limit tokens sufficient, balance covers margin
- Open/re-place: `open_order` (ETH_USDT, sell, 2, market_type=futures, type=limit, price=<level price>) — **must carry `client_order_id=eth-grid-<price>`**
- Place TP: `open_order` (ETH_USDT, buy, 2, futures, limit, price=fill price-2×step, reduce_only=true, client_order_id=eth-grid-tp-<fill price>) — reconcile TP order status every round: if flat and your own tp order is left over (because the user closed everything), use cancel_order to cancel your own eth-grid-tp-* order (your own prefix only)
- **Re-anchor**: cancel first, then place (§3.1), 30-minute cooldown; if a cancel fails, abandon re-anchoring for this round and retry next round
- Close/cancel: normally the agent does not need to act (TP closes automatically); on exceptions use `close_position` to close **only your own share of the level** by contract count (WARNING: positions are merged; be careful not to close the user's share)
- API error → skip the round, **never retry an order from an error state**
- Order rejected (e.g. risk cap) → record it, do not force it; if maxOpenPositions errors → report to the user to adjust the config
- Abnormal state (balance jump, position mismatch, order-view anomaly) → stop trading and record it
- Conservative first: **when unsure, do nothing**

## 6. Off limits

- ❌ **The user's ETH_USDT manual position (managed in the App, 10 contracts @1912 as observed on 08-19) and all App orders** — only touch things when topping up your own grid levels
- ❌ Everything belonging to XAUUSD CFD, SNDK_USDT and UNITREE_USDT — other agents' and engine strategies' territory
- ❌ `halt_trading` / `switch_direction` / strategy parameter changes / `modify_sl_tp`
- WARNING: `cancel_order`: only your own prefix eth-grid-* / eth-grid-tp-* orders may be cancelled; no other order may be cancelled
- ❌ Longs (short-only scope, mirrors SNDK)

## 7. How it runs

- Background sub-agent, ~10 rounds per turn (about 4-5 minutes) then it reports, and the main session automatically continues it → uninterrupted monitoring
- Runs **concurrently with the gold/SanDisk/Unitree agents**: separate sessions, separate documents, separate state files; shares engine risk control (global circuit breaker/daily loss — one trigger stops all, which is protection)
- The user can at any time: feed ideas/news (relayed by the main session), call a stop, adjust rules
- State persistence: `0_note/gate-trading/eth/james-MECHREVO/state/eth-agent-state.json`; trade post-mortems: `0_note/gate-trading/eth/james-MECHREVO/review/eth-review-YYYYMMDD-HHMMSS.md`
- Grid prices follow `list_futures_orders` (real exchange-side orders); the engine tracker view loses old orders after a restart
- WARNING: verify before deployment that the local engine binary includes the M23/M24 tools (`list_futures_orders`/`list_trigger_orders`); if missing, fall back to the exchange-side view of `get_orders` and report to the main session

## 8. Parameter mapping and history

### 8.1 SNDK v2 → ETH v1 parameter mapping (mirror relationship)

| Parameter | SNDK v2 (decided) | ETH v2 (decided) | Equivalence |
|---|---|---|---|
| step | 5 | **ATR-adaptive clamp(round(0.5×ATR15m), 3, 8)** | Now formally in effect (v1 actually used a fixed 5); late-night ATR 3.44 → step 3 |
| Level count | 12 | **12** | Same |
| Contracts per level | 2 (≈33 USD) | **2** (≈42 USD) | Same count; ETH has a higher per-contract notional |
| Anchor offset | mid + 5 | mid + 1×step | 1×step |
| Per-level take-profit | fill price - 10 | fill price - 2×step | 2×step |
| Re-anchor-down threshold | lower edge - 15 | lower edge - 3×step | 3×step |
| **Trend gate (new in v2)** | none | **place new levels/re-anchor only under 15m EMA bearish alignment; under bullish alignment manage existing positions only** | Post-mortem lesson: shorting in an uptrend = catching a falling knife |
| **Spike freeze (new in v2)** | none | **state.grid.spike (1m move > max(1%, 3×ATR15m)) → freeze new levels for 30 minutes** | Post-mortem lesson: the 04:51 spike through the top gained +98 points in 2 minutes |
| Protection line A | top + 20 | top + **2×step**, **triggers as soon as a 15m or 1m close crosses the line** | v1's 4×step left no reaction time |
| Protection line B | unrealized loss ≤ -30 USD | unrealized loss ≤ **-30 USD** | Same |
| Daily-loss stop | ≤ -10 USD | ≤ **-10 USD, reset at 08:00 Beijing time** | Scope decided |
| Re-anchor cooldown | 30 minutes | 30 minutes | Same |
| Direction | short only | short only | Same |

### 8.2 Scale-difference note (please review before sign-off)

- ETH's price scale (≈2097) is 1.3x SNDK's (≈1612), but its 1m volatility (ATR15m 2.87) is far smaller than SNDK's (about 10): under the same formula **step 3 is only 0.14% of price**, whereas SNDK's step 5 is 0.31%. If you want the "relative volatility feel" aligned with SNDK, you can widen the step floor (e.g. clamp 6~16 → currently computes to ~7); **keeping the formula unchanged = an absolute step on the tight side, with more frequent fills but thinner per-level profit**. Choose one of the two; the user decides.
- The full grid notional ≈ 500 USD is about 9% of the engine's maxSymbolNotional (5500), the same order of magnitude as the SNDK grid (≈400 USD); 12 fully filled levels = 12 independent positions, so re-check `maxOpenPositions` before deployment (it was 4→40 for SNDK at the time, now configured as 4).
- Funding -0.00013 (shorts receive) + maker rebate: the cost of holding is negative, which favors holding a grid.

### 8.3 History and changes

- **v2** (08-21, decided after the post-mortem): trend gate (place levels only under 15m EMA bearish alignment) + spike freeze (state.grid.spike) + protection line A moved forward (2×step + 1m trigger) + ATR-adaptive step formally in effect + daily-loss reset at 08:00 Beijing time; engine 9103 budget fixed (12000/6500), eth_scalper signal source enabled (signal_only)
- **v1** (08-20): created as a mirror of SNDK v2; deployed live 08-19 23:16 → closed at 04:51 at -39.2 when the user's App close also closed it (post-mortem `review/eth-review-20260821-grid-v1.md`)
- **Lessons inherited from SNDK** (permanent): v1's fixed zone ran 18 h/2847 rounds with only 1 fill → v2 switched to a dynamic rolling anchor; price_orders only supports full close (1017/1014/1021/1038) → place_trigger_order strictly forbidden; get_market ticker cache-lag bug (judge prices from the order book); the tracker view drops orders on restart → always use the exchange-side view
