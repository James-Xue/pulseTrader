# SNDK SanDisk Perpetual Sub-Agent — Taskbook (home machine james-MECHREVO edition)

> Read this file at the start of every batch; update it when done. Task status: ⬜ todo / 🔄 in progress / ✅ done
> This machine takes over the company machine's grid (same prefix t-sndk-grid-*); the company machine is offline, so exchange-side orders are the single source of truth

## Current tasks (grid v2.5 expansion mode, taken over in the early hours of 2026-08-20)

1. **✅ Takeover**: the exchange holds 13 short orders at 1600~1660 (step 5, 2 contracts/level) + 5 TP buy orders (1555~1575, corresponding to filled levels 1565~1585), position 0
2. **🔄 Grid maintenance (~25 s per round)**: `python3 sndk_grid.py status` → reconcile → act:
   - **Place a TP for a filled level**: a grid short filled and has no matching TP → `python3 sndk_grid.py tp <fill price>` (reduce_only buy @fill price-10, 2 contracts)
   - **Re-place after a TP fill**: a TP buy filled and the current price is below that level → `python3 sndk_grid.py place <level price>`
   - **Top up a missing level**: one of the 12+ levels has no order and mid has not risen above that level → top it up
3. **🔄 Expansion-mode following (v2.5 field-tested tonight, zero-cancel principle)**:
   - Price moving up: the top level lags the current price by >5 points → add a level at the top in steps of 5, with top level ≈ round(last+5, 5)
   - Price moving far below (mid < lowest level-15) → open a new level below to catch the bounce
   - The level count is dynamic; do not cancel orders (unless the protocol explicitly requires it)
4. **🔄 Protection line (autonomous overnight mode, see the next section)**: A: 15m close > top level+20; B: grid unrealized loss ≤ -30 USD
5. **⬜ Red-window execution (tonight's focus)**: see the "Red-window protocol" below

## 🌙 Autonomous overnight mode (2026-08-20 00:5x ~ 08:00 Beijing, user asleep, do not disturb)

- **Protection line B (grid unrealized loss ≤ -30 USD) triggered → execute automatically**: fully close the grid position (reduce_only order) + cancel all t-sndk-grid-* orders → **dormant for the rest of the day** (place no new orders at all; leftover TP orders may be cancelled) → record + alert (for the archive, no reply expected). Rationale: the user cannot respond, so "recommend full close" is applied autonomously per the user's preset threshold.
- **Protection line A (15m close > top level+20) triggered → do not auto-close**: stop all level additions, TP as usual, record + alert. If unrealized loss later reaches the B threshold → execute automatically per B.
- **A sharp rally through the grid inside the red window**: do not wait for the user; execute per the B threshold.
- **Daily-loss stop**: the grid's own realized loss today ≤ -10 USD → stop opening new orders (as usual, autonomous).
- **Other strategy adjustments**: decide autonomously and record the reason on the thinking board (authorization unchanged).
- **Communication with the main session**: alerts/briefs are still sent (for the archive); do not expect any reply; never abandon a protective action on the grounds of "waiting for the user".

## ⛔ Red-window protocol — FOMC meeting minutes (2026-08-20 02:00 Beijing, red event)

- **Window**: 01:30 ~ 02:45 Beijing time (2026-08-19T17:30Z ~ 18:45Z UTC)
- **Forbidden inside the window**: any new short orders (level additions/re-placements/top-ups all paused); re-anchoring
- **Allowed inside the window**: TP buy orders as usual (reduce-only direction); watch existing orders; run protection-line checks more frequently
- **Anomaly inside the window**: price spiking up through the grid → protection line A/B alerts immediately and waits for the user
- **Before/after the window**: the last round before 01:30 takes a snapshot (price/position/unrealized PnL); the first round after 02:45 sends an event post-mortem brief to the main session

## Hard red lines (immutable, even the "autonomous strategy update" authorization does not override them)

1. Short only; 2 contracts per level; daily-loss stop: the grid's own realized loss today ≤ **-10 USD** (counted by UTC day) → stop opening new orders
2. **Do not touch the user's App orders** (the cancel script has a built-in prefix check; only t-sndk-grid-* can be cancelled)
3. Do not auto-close the user's positions; do not touch other symbols (XAUUSD/BTC/UNITREE/ETH etc.)
4. Network/API failure → skip the round, never place orders based on stale data; 3 consecutive failed rounds → alert

## Autonomous strategy-update authorization (verbal authorization from the user at 2026-08-20 00:5x)

- Adjustable: level-addition cadence/max level count/protection-line thresholds/red-window rule details — **must** record the reason on the thinking board
- Not adjustable: the hard red lines (previous section); the -10 TP distance must be confirmed with the user before any change
- Every strategy adjustment also updates the "strategy change log" section of this taskbook

## How it runs

- Tools: `0_note/gate-trading/sndk/james-MECHREVO/tools/sndk_grid.py` (direct REST, falling back to the proxy automatically when direct fails)
- State: `…/state/sndk-agent-state.json`; thinking board: `…/thinking-board/sndk-agent-thinking.md` (newest first)
- 10 rounds per batch; at the end of each batch a SendMessage brief to the main session; protection-line/anomaly events reported immediately
- The parent-session watchdog cron checks state freshness every 10 minutes and automatically takes over after 6 minutes of staleness
- The engine view is reference only: this machine's engine is the CFD build (5daeaae), so everything SNDK follows the exchange REST

## Strategy change log

- 2026-08-20 00:5x: took over the company machine's v2 grid and adopted the expansion-mode strategy field-tested tonight (v2.5); added the FOMC red-window protocol (02:00 Beijing)
- 2026-08-20 05:2x: the company machine came back online as a second writer (events #1/#2 phantom-TP shaved closes, #3 correct TP placed twice). This machine's agent switched to **protector mode**: stop all new orders (place/tp), only monitor + protection line B auto full close + the daily-loss red line; do not top up missing TPs (let the company machine reconcile and fix); only after 3 consecutive rounds with no external activity and an existing gap, top up one order and then return to protector mode; do not cancel or re-place duplicates; do not retaliate against the company machine's v2 re-anchor cancellations, record + alert. Protection line B has the highest priority.
- 2026-08-20 06:5x: the company machine's reconciliation has been correct for consecutive rounds (event #6 duplicate @1595, #7 top-up tp-1610@1600), so reconciliation authority is handed back. **The maintenance threshold is raised to: company machine silent ≥30 minutes** (the state file records last_external_activity) + a gap exists + no duplicate orders + the current price is ≥5 points away from the target TP price; only when all four conditions hold at once, top up one order; otherwise stay a strict protector.
