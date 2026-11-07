# SNDK SanDisk Contract Auto-Trading Sub-Agent — Strategy Document v2

> **v1** (2026-08-18, first grid version, fixed range 1730-1800) → **v2** (2026-08-19 post-mortem upgrade: dynamic rolling anchor + ATR stepping + an overall protection line) · Runs on the pulseTrader engine (mainnet live) · This file is the rule baseline for the SNDK sub-agent; review it before making any change
> It follows the pattern of the gold agent `xauusd-subagent-strategy.md`, but the risk parameters are completely different — **do not copy the gold numbers**

## 1. Overview

Run a long-lived background sub-agent on the pulseTrader engine that executes a **short-chasing grid v2** on SNDK_USDT (Gate.io perpetual futures):

- **Short only** (authorised by the user; never go long, never fight the trend to catch a bounce)
- **Dynamic rolling anchor** (the core of v2): the grid zone is not fixed, it moves with the market. Anchor A = round(mid price + 5, step), grid zone = [A, A + 20×step]; when price moves away from the grid, the whole grid is re-placed (§3.1)
- **ATR-adaptive stepping** (v2): step = clamp(round(0.5 × ATR15m), 3, 8), initially **5 points**; 12 cells (initial zone width 60 points; **at 08-19 20:4x the user widened it: 20 cells / 100-point zone**, see §8)
- **2 limit short orders per cell** (1 contract = 0.01 SNDK ≈ 16.7 USD notional @1670; 2 contracts ≈ 33 USD per cell, total grid notional ≈ **660 USD**), rolling with the anchor
- **Each cell carries its own take-profit**: right after a fill, place a reduce-only limit buy order, limit = that cell's fill price - 10 points (2 steps). ⚠️ Gate price_orders trigger orders do not support partial closes (tested: 1017/1014/1021/1038); **place_trigger_order is strictly forbidden**, TP always goes through `open_order` as a reduce-only limit buy
- **No stop-loss per cell** (user requirement); **an overall protection line for the grid** (new in v2, §4)
- **Cyclic re-placement**: when a cell's TP is filled and the current price is below that cell, automatically place a new short order + new TP at the same price (classic grid)
- One round takes about 25–30 seconds: read live state → verify working orders → top up / re-place → check fills → place TPs → re-anchor check → protection-line check → risk check
- Every close writes a post-mortem md into `0_note/` (`sndk-review-YYYYMMDD-HHMMSS.md`)

## 2. Market and cost background

| Item | Value |
|---|---|
| Trading direction | SNDK_USDT futures (`market_type=futures`; after M22 manual orders can be executed under the cfd active direction) |
| Contract spec | 1 contract = 0.01 SNDK (quanto 0.01; the risk gate uses the true notional) |
| Market data | 1m klines + ticker + order book (all futures channels) |
| Fees | Futures taker 0.05% / maker 0.02% (2 contracts ≈ 33 USD notional, one-way taker ≈ 0.017 USD) |
| Current state (08-19 20:52) | v2.1 grid: 20 limit short cells 1615~1710 step 5, 2 contracts/cell; 12 cells (1615~1670) already filled into a position + six cells 1685~1710 open (1675/1680 already filled); 14 reduce-only TPs on the book; mid ~1680, funding rate +0.0001 |
| Engine strategies | The 3 SNDK strategies (momentum/mean_reversion/supertrend, signal_only) are running |

## 3. Grid operating protocol (the core)

**Per-round checklist** (`list_futures_orders SNDK_USDT` = every working order on the exchange side; `list_trigger_orders` = trigger orders; judge prices with the order-book mid = (best_bid+best_ask)/2 — ⚠️ the ticker.last from get_market has a caching lag bug, so do not use it to judge prices):

1. **Top up missing cells**: if one of the 20 cells has no working order (not re-placed after a fill / cancelled) and mid has not yet risen past that cell → place a new `sndk-grid-<price>` limit short of 2 contracts
2. **Fill → place TP**: when a cell fills (its `t-sndk-grid-*` disappears from list_futures_orders and the price has touched or exceeded that cell) → immediately `open_order`: symbol=SNDK_USDT, side=buy, quantity=2, market_type=futures, type=limit, price=fill price-10, **reduce_only=true**, client_order_id=sndk-grid-tp-<fill price>. ⚠️ place_trigger_order is strictly forbidden; a missing reduce_only is strictly forbidden (it would open an opposite position)
3. **TP filled → re-place**: when the exchange-side `t-sndk-grid-tp-<fill price>` order disappears (filled) and mid is below that cell → place a short order again at the same cell price (cycle)
4. **Re-anchor check** (§3.1, every round)
5. **Overall protection-line check** (§4, every round)
6. **Do not interfere**: the user's App orders — look, never touch

### 3.1 Re-anchor protocol (v2 dynamic rolling anchor)

- Anchor A = round(mid + 5, step), grid zone = [A, A + 20×step]; short only → the grid zone always sits above the current price waiting for a bounce
- **Re-anchor triggers (any one)**:
  - a) mid < the grid's lower edge A - 15: price has moved far below the grid (the downtrend continues and the grid idles) → move down to follow
  - b) mid > the grid's upper edge + 5: price has broken up through the grid → **do not re-anchor automatically**; switch to protection line A (the protection line manages the unrealised loss of filled cells)
- **Cooldown**: at least 30 minutes since the last re-anchor (anti-jitter, to avoid repeated cancel/re-place in a choppy market)
- **Action**: first cancel all of your own `sndk-grid-*` short orders and any leftover `sndk-grid-tp-*` (your own prefix only) → confirm they are all gone → place 20 cells upward from the new anchor's lower edge (per cell `open_order`: sell, 2, futures, limit, price=<cell price>, client_order_id=sndk-grid-<price>)
- Persist the state after re-anchoring, and record the old and new anchors on the thinking board

## 4. Exit rules

> SNDK futures have **no exchange-native attached SL/TP** (open_order's sl_price/tp_price are CFD only); take-profit is a per-cell **reduce-only limit buy** (resident on the exchange, effective even if the engine crashes; when the price is touched it closes only that cell's 2 contracts). price_orders trigger orders only support **closing the whole position**, so the grid does not use them. **No per-cell stop-loss** (user specified).

| Rule | Trigger | Action |
|---|---|---|
| Per-cell take-profit | price ≤ that cell's fill price - 10 | the reduce-only limit buy automatically closes just that cell's 2 contracts |
| **Overall protection line A** (v2) | 15m close > grid top + 20 points | push an alert + recommend closing everything; **the user decides whether to close** (never automatic) |
| **Overall protection line B** (v2) | cumulative grid unrealised loss ≤ **-30 USD** | push an alert + recommend closing everything; **the user decides whether to close** |
| Daily-loss stop | cumulative realised loss today ≤ **-10 USD** | no new cells / re-placements for the rest of the day (existing grid is managed as-is) |

## 5. Risk and operating protocol

- Confirm every round: `trading_halted=false`, rate-limit tokens available, balance sufficient for margin
- Open / re-place: `open_order` (SNDK_USDT, sell, 2, market_type=futures, type=limit, price=<cell price>) — **must carry `client_order_id=sndk-grid-<price>`**
- Place TP: `open_order` (SNDK_USDT, buy, 2, futures, limit, price=fill price-10, reduce_only=true, client_order_id=sndk-grid-tp-<fill price>) — verify the TP order state every round: if there is no position but your own tp order is left over (caused by the user closing everything), use cancel_order to cancel your own sndk-grid-tp-* orders (your own prefix only)
- **Re-anchor**: cancel first, then place (§3.1), 30-minute cooldown; if a cancel fails, abandon the re-anchor for this round and retry next round
- Close / cancel: normally the agent does not need to act (TP closes automatically); in abnormal cases `close_position` closes **only your own share of that cell, by contract count** (⚠️ positions are merged, so be careful not to close the user's share)
- API error → skip this round, **never retry an order from an error state**
- Order rejected (e.g. risk limit) → record it, do not force it; if maxOpenPositions errors → report to the user to adjust the config
- Abnormal state (balance jump, position mismatch, inconsistent order view) → stop trading and record it
- Conservative first: **when unsure, do nothing**

## 6. Off-limits

- ❌ The user's manual SNDK position and all App orders — only touch your own grid cells when topping them up
- ❌ Everything related to XAUUSD CFD, BTC_USDT, UNITREE_USDT, **ETH_USDT (including orders with the eth-grid-* prefix, another party's grid added after the 23:22 engine restart)** — that is the territory of other agents and engine strategies
- ❌ `halt_trading` / `switch_direction` / strategy parameter changes / `modify_sl_tp`
- ⚠️ `cancel_order`: only allowed for your own sndk-grid-* / sndk-grid-tp-* orders; no other order may be cancelled
- ❌ Going long (the short-chasing authorisation covers the short direction only)

## 7. How it runs

- A background sub-agent that runs ~10 rounds per turn (about 4–5 minutes) and then reports, with the main session automatically continuing → non-stop screen watching
- Runs **at the same time as** the gold / Unitree agents: each has its own session, its own document and its own state file; they share the engine's risk control (global circuit breaker / daily loss — one fires and everything stops; this is protection)
- The user can at any time: feed in ideas / news (relayed by the main session), call a stop, adjust the rules
- State persistence: `0_note/gate-trading/sndk/joey-Z170I-PRO-GAMING/state/sndk-agent-state.json`; trade post-mortems: `0_note/gate-trading/sndk/joey-Z170I-PRO-GAMING/review/sndk-review-YYYYMMDD-HHMMSS.md`
- Grid prices are taken from `list_futures_orders` (the real exchange-side working orders), because the engine tracker view loses old orders after a restart

## 8. History and changes

- **v1** (08-18): 36 cells over a fixed range 1730~1800, step 2, 2 contracts/cell. In 18h / 2847 rounds only 1 fill happened (1730×2) and the TP was never triggered in practice — a fixed range cannot keep up with a one-way market; see the post-mortem in `review/sndk-review-20260819-131500.md`
- **v2** (08-19): dynamic rolling anchor (§3.1) + ATR-adaptive stepping (step=0.5×ATR15, 3~8) + overall protection line (§4); cells 36→12, zone width 60 points
- **v2.1** (08-19 20:4x): the user instructed widening the grid range — cells 12→**20**, zone **1615~1710**, protection line A = top + 20 = **1730**; line B / TP / re-anchor protocol unchanged (task brief item 8)
- Platform lessons (permanent): price_orders only supports closing the whole position (1017/1014/1021/1038) → engine M24 now rejects size>0; the get_market ticker caching lag bug (unfixed; judge prices from the order book); the tracker view loses orders on restart → always use the exchange-side view
- The first anchor and parameters take effect after the user confirms them; the confirmation record is in the task brief
