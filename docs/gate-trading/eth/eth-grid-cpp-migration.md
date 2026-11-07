# ETH Grid v2 Text Strategy → C++ Implementation Mapping

> Cross-references `strategy/eth-subagent-strategy.md` (v2 spec, `docs/gate-trading/eth/james-MECHREVO/strategy/`) with `src/grid/GridManager.{hpp,cpp}`.
> Status: ✅ implemented in the engine · ⏳ not migrated (still handled by the sub-agent/toolchain) · 🗑️ removed
> Last verified: 2026-08-21 (M27 PR-6/PR-7, 867 green)

## Rule mapping

| # | v2 text rule (§) | C++ implementation point (src/grid/GridManager.cpp) | Status |
|---|---|---|---|
| 1 | Short only, 2 limit shorts per level (§1) | `hangLevels()` — sell limit, `m_cfg.qty_per_level` | ✅ |
| 2 | Anchor A = round(mid + 1×step, step), grid zone = [A, A+12×step] (§3.1) | `recomputeLevels()` (anchor_offset_steps=1) | ✅ |
| 3 | step = clamp(round(0.5×ATR15m), 3, 8) (§3.1) | `computeAtr15m()` + `recomputeLevels()` (step_mode="atr") | ✅ |
| 4 | a) mid < anchor-3×step → re-anchor downward, 30 min cooldown (§3.1a) | `decideAction()` Reanchor branch + `reanchor()` (lower_reanchor_steps=3) | ✅ |
| 5 | b) mid > upper edge+1×step → **do not auto re-anchor**, switch to protection line A (§3.1b) | `upper_reanchor_steps` field removed (PR-6), path goes to ProtectA | ✅ 🗑️ |
| 6 | c) trend gate: place new levels/re-anchor only when the 15m EMA is bearish (§3.1c) | `readTrendGate()` — SignalBoard `eth_scalper_ETH_USDT` trend_state, wall-clock freshness ≤120 s, stale=freeze | ✅ |
| 7 | d) spike freeze: 1m move > max(1%, 3×ATR15m) → freeze **new levels + re-anchoring** for 30 min (§3.1d) | `spikeTripped()` + `markSpikeIfNeeded()` + decideAction re-anchor branch gate (PR-6 fix) | ✅ |
| 8 | Fill → place reduce-only limit buy TP = fill price-2×step, place_trigger_order strictly forbidden (§3.2) | `manageTp()` — reduce_only limit buy, `tp_distance_steps=2` | ✅ |
| 9 | TP filled → book it + re-place cyclically when mid<level price (§3.3) | `manageTp()` + `recordTpFill()` (realized booking + daily-loss check) | ✅ |
| 10 | Protection line A: 1m/15m close > top+2×step → reduce-only market close of **exactly the grid share** + decide re-placement per §3.1 (§4) | `decideAction()` ProtectA + `flattenGridShare()` (share = Σfilled × qty, never exceeds the share; re-place gate PR-6) | ✅ |
| 11 | Protection line B: grid unrealized loss ≤ -30 USD → same as A (§4) | `decideAction()` ProtectB + `flattenGridShare()` | ✅ |
| 12 | Daily loss ≤ -10 stop: no new positions/re-placements, existing positions managed as-is; reset at 08:00 Beijing time (§4) | `dailyLossFrozen()` + `tickFast()` day boundary (UTC 00:00 == Beijing 08:00) + `GridAction::DailyStop` (wired in PR-6) | ✅ |
| 13 | Cancellation limited to your own eth-grid-* prefix (§5/§6) | `cancelAllGridOrders()` (prefix match) | ✅ |
| 14 | The exchange-side order view is the truth (a restart loses the tracker) (§7) | `reconcileExchange()` (IGridGateway::openFuturesOrders) | ✅ |
| 15 | Engine crash recovery: automatically resumes after a restart | `loadState()`/`saveState()` schema 2 — phase/tp_resting/geometry restored (PR-7); the first slow pass after a restart reconciles first | ✅ |
| 16 | TP orders that survived a crash → adopt, do not re-place | `reconcileExchange()` adopt branch (matches the level by TP price) | ✅ |
| 17 | Direction-switch cancellations count as "cancelled", not "filled" | `onDirectionSwitched()` → `m_externalCancelPending` | ✅ |
| 18 | Do not touch the user's manual position; refuse to start if a non-grid position exists (force overrides) (§6) | `refreshUserPositionWarn()` + `start()` pre-check | ✅ |
| 19 | Write a post-mortem md for every closed trade (§1) | ⏳ Not migrated — replaced by engine info logs + realized booking | ⏳ |
| 20 | Mechanical watchdog state JSON (eth_watch.py) | ⏳ Superseded by GridManager, eth_watch.py pending retirement | ⏳ |

## Grid control plane (REPL / JSON-RPC / MCP, three channels)

| Command | Effect | Error code |
|---|---|---|
| `grid start [--levels N] [--qty Q] [--step S] [--anchor P]` | Start (parameter overrides take effect permanently and survive restarts) | 9201 already running / 9101 invalid parameter |
| `grid status` | Full snapshot (phase/anchor/step/top/per-level/PnL/gates/freeze) | — |
| `grid pause` | Pause: orders stay on the exchange, no new actions (persisted) | 9101 not running |
| `grid resume` | Resume Paused → Running (the next slow pass reconciles first) | 9202 not paused |
| `grid stop` | Stop and cancel all eth-grid-* orders | — |

## Parameter mapping (GridConfig ↔ v2 spec)

| GridConfig | Default | v2 spec | Notes |
|---|---|---|---|
| levels | 12 | 12 levels | |
| qty_per_level | 0.02 | 2 contracts (0.02 ETH) | |
| step_mode / step_atr_mult / step_min / step_max | atr / 0.5 / 3 / 8 | clamp(0.5×ATR15m, 3, 8) | |
| atr_period | 40 | ATR15m (mean 1m TR over 40 bars) | |
| tp_distance_steps | 2.0 | fill price - 2×step | |
| anchor_offset_steps | 1.0 | anchor = mid + 1×step | |
| lower_reanchor_steps | 3.0 | lower edge - 3×step | |
| protect_line_a_steps | 2.0 | top + 2×step | upper_reanchor_steps removed |
| protect_line_b_usd | -30.0 | ≤ -30 USD | |
| daily_loss_limit_usd | -10.0 | ≤ -10 USD | |
| daily_reset_hour | 8 | 08:00 Beijing time | |
| reanchor_cooldown_min / spike_freeze_min | 30 / 30 | 30 / 30 min | |
| spike_pct / spike_atr_mult | 0.01 / 3.0 | max(1%, 3×ATR15m) | |
| force | false | — | refuses to start when a non-grid position exists |

## Engine migration history

- **M27 PR-1~5** (fd8505c..f724fbb): GridManager landed (levels/TP/re-anchor/protection lines/gates/persistence schema 1/four control-plane commands)
- **M27 PR-6** (199b043): start overrides take effect, grid_resume, spike/daily-loss gates added to re-anchoring, DailyStop wired in, post-protection-line re-placement gate, upper_reanchor_steps removed, main.cpp tick compile fix
- **M27 PR-7** (e023979): persistence schema 2 (phase/tp_resting/geometry restore), pause persistence, [grid] TOML example section
- **TODO**: engine restart to take effect → testnet drill (place levels→fill→TP→cycle; protection lines; restart; direction switch) → enable on mainnet and retire eth_watch.py
