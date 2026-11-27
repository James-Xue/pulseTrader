# Paper Team Reconciliation Audit — 2026-11-27

> **Status: AUDIT COMPLETE — DO NOT APPLY THE FINDINGS' FIXES VERBATIM.**
> The numbers in this document were independently reproduced. The remediation
> plan as originally written was rejected by the integration review
> (`accept=false`); §6 is the corrected, staged gate to implement.
>
> Scope: PT paper engine (`~/1_Code/09_pulseTrader`) + GT runner/ledger
> (`~/1_Code/11_gold-trader-mind`).
>
> Evidence base: read-only snapshot at **2026-11-27 12:53 CST**
> (`/tmp/pulse-audit/`), plus live spot checks through 13:49 CST.
> Code at audit time: PT `c34d7dd`, GT `9b99f64`. Current PT HEAD `aa2503b`
> only adds the Orders-page strategy filter (unrelated to accounting).
>
> Audit team: 5 auditors (engine / GT ledger / balance / fees / code) +
> 24 fact-checkers + 24 risk reviewers + 3 integration reviewers = 56
> sub-agents. Model: opus. Depth: 1. Nested delegation: denied, none occurred.

---

## 0. How to use this document in the fixing session

1. Read §1–§4 for the verified facts and root causes.
2. **Do not copy §5's `proposed_fix` text into code.** §6 replaces it with the
   corrected gate; several original proposals rely on fields/events that do
   not exist or produce tests that are green on HEAD.
3. Work on **copies** of the databases. Never run a destructive migration or
   a retrofit against the live paper state/ledger while the engine or the GT
   runner is running.
4. Every new test must be **red on HEAD** (except explicit canaries) and
   become green only when the account identity holds.
5. Keep the running system untouched until the P1/P2 tests pass on a copy.

---

## 1. Executive summary

- **Current epoch (E3, after 2026-11-27 09:46 CST) reconciles to the cent.**
  At the 12:53 snapshot: 15 team fills, Σpnl = −7.70, Σfees = 0.42;
  account realized −7.70, fees 0.42; balance = 100 − 7.70 − 0.42 =
  **91.88** (cash 88.2425 + locked margin 3.6375); equity 92.86 with the
  open position's +0.98 unrealized.
- **The three earlier epochs do not reconcile.** Exact per-epoch gaps are in
  §2. Lifetime: account snapshots total −115.20 pnl / 6.78 fees; the engine
  `trades` table totals −99.22 / 6.18 → **−16.58 USDT gap**.
- The multi-day mismatch is **not one bug**. Five compounding causes:
  1. 18 venue exits were dropped (no trade row) → −2.49 pnl, −0.48 fees.
  2. PositionManager netting PnL was written onto **entry** rows
     (+7.58, cross-member) → per-member signs are wrong.
  3. Two `close_` rows stored fees = 0; a further 8 buy-side closes are
     missing → −0.60 fees lifetime.
  4. GT ledger history is not reproducible: pre-reset `paper_exit` fills were
     never ingested, entries were booked as exits, quanto was 1.0 (100×
     under), CFD sell legs were fee-estimated at 3.85 bp; the current ledger
     is missing one round trip because the engine keeps only 20 reports in
     memory.
  5. Four capital epochs (seed/top-up/resets) are not recorded anywhere;
     `paper-state.json` was hand-edited. No query over trades alone can
     reproduce the balance across resets.
- **Only 3 of 5 configured members ever traded**: `xau_supertrend`,
  `xau_bouncy`, `xau_v10`. `xau_15m_trend` and `xau_sunny` have 0 rows in
  `trades-paper.db`.

---

## 2. Epoch contract and account identity

Use exactly these local-time (Asia/Shanghai) boundaries in every test:

| Epoch | Window (CST) | Rows | Engine Σpnl / Σfees | Account realized / fees | Gap (pnl / fees) |
|---|---|---|---|---|---|
| **E0** | 2026-11-25 15:32:15 → 11-26 10:30:00 | 63 (ids 1–63) | −26.88 / 1.86 | −32.79 / 1.86 | **−5.91 / 0.00** |
| **E1** | 11-26 10:30:00 → 11-27 09:23:00 | 130 (ids 64–194; **id 64 missing**) | −64.48 / 3.90 | −74.55 / 4.44 | **−10.07 / −0.54** |
| **E2** | 11-27 09:23:00 → 09:46:00 | 2 (ids 195–196) | −0.16 / 0.00 | −0.16 / 0.06 | 0.00 / **−0.06** |
| **E3** | 11-27 09:46:00 → 12:53 snapshot | 15 (ids 197–211) | −7.70 / 0.42 | −7.70 / 0.42 | **0.00 / 0.00** |
| total | — | 210 (ids 1–211, id 64 missing) | −99.22 / 6.18 | −115.20 / 6.78 | **−15.98 / −0.60** |

Account identity (must hold per epoch, flat or with open positions):

```
cash + locked_margin = seed + realized_pnl − fees_paid
equity               = cash + locked_margin + unrealized_pnl
```

Snapshot evidence:

| Snapshot | cash | realized | fees | locked | notes |
|---|---|---|---|---|---|
| `paper-state.json.bak-20261126-100usd` | 9965.35 | −32.79 | 1.86 | 0 | pre-first-reset |
| `paper-state.json.bak-20261127-pre-topup` | 21.01 | −74.55 | 4.44 | 0 | pre-second-reset |
| `paper-state.json.bak-20261127-pre-final-reset` | 99.78 | −0.16 | 0.06 | 0 | E2 |
| 12:53 snapshot | 88.2425 | −7.70 | 0.42 | 3.6375 | E3 |
| live 13:39–13:49 | 92.19 | −7.15 | 0.66 | 0 | flat; equals 100 − 7.15 − 0.66 |

E0 caveat: at 15:45:49 a `FirstRun` re-seed wiped an earlier +1.81, so the
complete log is −30.98; the state −32.79 includes that explicit re-seed
event. Do not assert `Σtrades.pnl == realized` for E0 without recording it.

Other data-integrity facts:
- `trades` spans ids 1..211 with **id 64 absent**; 6 duplicate-`order_id`
  INSERTs were dropped in the paper app log.
- `client_order_id` is **not unique**: 27 ids repeat (81 rows, max 8) because
  each member's sequence restarts on every engine restart/epoch.
- Before 2026-11-26 11:46, all rows (including 29 team entries) were stored
  with `strategy_name='momentum_scalper'`. The built-in strategy never
  traded; this is a label bug, not a second strategy.

---

## 3. What each member actually did (E1, venue-attributed)

Recorded venue closes (`paper_exit_*` + `close_XAUUSD*`), E1:

| Member | Entries (buy/sell) | Exits | Gross | Entry fees | Exit fees | Net (recorded fees) |
|---|---|---|---|---|---|---|
| `xau_supertrend` | 52 (27/25) | 44 | **−68.330** | 1.62 | 1.20 | −71.150 |
| `xau_bouncy` | 10 (7/3) | 5 | **−3.540** | 0.42 | 0.00 | −3.960 |
| `xau_v10` | 12 (10/2) | 7 | **−0.190** | 0.60 | 0.06 | −0.850 |
| recorded total | 74 | 56 | **−72.06** | 2.64 | 1.26 | — |
| 18 unrecorded venue closes | — | — | **−2.49** | — | 0.48 (8 short closes) | — |
| **venue total** | 74 | 74 | **−74.55** | — | **4.44** | — |

Two per-member views exist because 7 closes were recorded on **entry** rows:

1. **Recorded-close view** (table above): supertrend −68.33, bouncy −3.54,
   v10 −0.19, unallocated −2.49.
2. **Full venue attribution** after reassigning the 7 netting PnLs to the
   closed position's owner: supertrend **−62.74**, bouncy **−3.54**,
   v10 **+1.80**, unallocated **−10.07** (sum −74.55).

The engine's stored member tags give `xau_bouncy +4.40` and `xau_v10 −2.70`
for the full history — **wrong sign and magnitude** for bouncy because the
+7.58 netting PnL is credited to the entering member. Do not use the stored
tag for per-member PnL.

E3 per-member at 12:53: supertrend −5.55 gross (6 trips), bouncy −2.15
(09:56 pair), v10 0; Σ −7.70 / fees 0.42. The GT ledger at 12:53 shows the
6 supertrend trips (net −5.91) but **misses the bouncy pair** (−2.21),
because the engine restart at 10:29:48 left only reports from 10:42 onward
in the in-memory ring.

---

## 4. Root causes and exact code locations

### 4.1 Venue exits dropped (18 positions, −2.49 PnL, −0.48 fees)

`EngineServices::tickPaperVenue` warns and `continue`s when PositionManager
no longer holds the `exchange_position_id`:

- `src/control/EngineServices.cpp:2879-2894`
- Same path for `recordExternalClose`; the venue has already realized the
  PnL and charged the fee.

Missing E1 position ids: `pp-154, 198, 202, 204, 206, 208, 210, 214, 216,
218, 220, 226, 228, 230, 232, 248, 250, 252` (8 short → 0.48 close fees,
10 long → no fee). 17 of them appear as "paper exit for unknown venue
position" warnings; pp-154 predates the log.

### 4.2 Netting PnL on entry rows / cross-member attribution (+7.58)

`OrderFlowExecutor::onOrderComplete` closes **any** opposite position of the
same symbol, ignoring `strategy_id`/member, while PaperVenue keeps one
position per order:

- `src/control/OrderFlowExecutor.cpp:769-799`
- `src/execution/PaperVenue.cpp:834-855` (always opens a new position)
- 7 entry rows carry PnL: paper-cfd-205 +3.46, 209 +1.99, 215 −1.86,
  219 +1.90, 227 +1.56, 231 +1.18, 251 −0.65; sum **+7.58**.
- Cross-member: these 7 closes belong to other members; a member's next
  entry can close a peer's engine position at the entry price.

### 4.3 Fee gaps (−0.60 lifetime; E1 −0.54)

- `close_` reports hard-code `fees = 0.0` at `src/control/EngineServices.cpp:1826/1832`,
  although `PaperCfdClose::fee` is computed at
  `src/execution/PaperVenue.cpp:640-641`.
- Two zero-fee buy rows: `close_XAUUSD_Sell_29` (2026-11-27 07:04:17) and
  `close_XAUUSD_Sell_1` (09:45:53) → 0.12.
- The other 0.48 comes from the 8 missing short-close buy legs in §4.1.
- Venue truth: 113 buy legs × 0.06 = **6.78**; DB 103 charged = 6.18.
- The correct formula is `fees == 6.0 * filled_qty` (filled_qty in lots;
  0.01 lot → 0.06). `6.0 * filled_qty / 100` is 100× wrong.

### 4.4 GT ledger: pairing, quanto, fee model, report window

- `Ledger._apply_fill` closes any open row on an opposite-side fill,
  including an **entry** (`is_exit=False`): `app/ledger.py:327-460`.
  PaperVenue never nets, so a missing exit makes the next entry the exit.
- Pre-reset ledger (45 trips): 0 of 108 fills are `paper_exit_*`; 43/45
  exits are the next `xau_team` entry; only 2 use a genuine `close_`.
  Reported −4.8840 vs venue −115.33 (window ±3).
- Quanto: all 45 trips were booked at `quanto_multiplier=1.0` (exactly 100×
  too small for 0.01 lot = 1 oz). Rebuild at 100 gives 46 trips / −174.04,
  not 45 / −156.67 (pairing difference).
- Fee estimator: 50/108 pre-reset fills were estimated at 3.85 bp on
  **CFD sell legs**, inflating fees 22.9%; with quanto=100 it would inflate
  ~21×. CFD must use 6 USDT/lot on the BUY leg only.
- Current ledger gap: `paper-cfd-261` / `paper_exit_pp-262_1` (−2.15 gross,
  0.06 fee, −2.21 net) is absent. Root: `PaperVenue` reports live only in
  memory (`src/execution/PaperVenue.hpp:305`), `recentReports(20)`
  (`src/control/EngineServices.cpp:858`), and the GT runner reads only
  `get_orders().recentReports` (`app/runner.py:475-536`).
- `get_trade_history` exists but returns the **oldest** page (iterates
  `all.rbegin()` at `src/control/EngineServices.cpp:1040`), has no
  `exchange_position_id`, and no from/to filter. A naive backfill would
  omit the 10 newest rows and orphan exits.

### 4.5 Capital resets are not recorded

- No reset/top-up RPC and no `capital_events` table; `PaperAccount::adjustCash`
  has no production caller.
- The 2026-11-26 10:30 and 11-27 09:46 resets were hand edits of
  `data/paper-state.json`; only the `paper-state.json.bak-*` files and log
  timestamps record them.
- `PaperAccountStore` saves every 1000 ms and on clean shutdown; it is a
  lossy snapshot, not a journal.
- Consequences: the balance cannot equal lifetime deposits + PnL without
  explicit capital events; cumulative cross-epoch comparisons are invalid.

### 4.6 Supporting defects

- Market orders leave `OrderRequest.price=0`; `PositionManager::reserveNotional`
  computes `qty*price*quanto = 0` and approves. The real notional is checked
  only post-fill (`PositionManager.cpp:689-760`, `OrderFlowExecutor.cpp:807-827`),
  which left 4 venue legs with no engine position (pp-154/198/204/248; 14.35
  USD margin/fee unrecorded in trades/GT).
- `client_order_id` duplicates across restarts (27 ids / 81 rows).
- `strategy_name` is not derivable from `client_order_id` for engine CFD
  signal orders with empty client ids (39 rows).
- `PaperVenue::recentReports` is ascending while `OrderTracker::recentReports`
  is descending; GT `_ingest_fills` does not sort → same-batch entry+exit can
  be processed exit-first.
- Cancelled/rejected terminal reports can contaminate positions/PnL
  (`PositionManager` current_price=0, notional=0; `getStrategySummary` has no
  status filter).
- `maxPositionNotionalCfd=8000` is a **per-market total**, despite the
  "per-position" comment; four concurrent 0.01-lot legs need ≥ ~19k and the
  team cap is 4.

---

## 5. Confirmed findings (24)

All 24 reproduced in Round 1. Severity is the audit's; the original
`proposed_fix` is deliberately not copied — see §6 for the corrected gate.

| # | Sev | Area | One-line finding | Key numbers |
|---|---|---|---|---|
| 1 | blocker | engine PnL | E1 trades understate account realized PnL | Σ −64.48 vs −74.55; −2.49 unrecorded + 7.58 entry PnL |
| 2 | blocker | engine fees | Fee column incomplete | E1 3.90 vs 4.44; −0.54 = 0.48 + 0.06 |
| 3 | blocker | engine exits | 18 venue positions have no exit row | pp ids above; −2.49; 8 short/10 long |
| 4 | blocker | GT ledger | Current ledger misses bouncy 09:56 | −2.21; ledger −5.91 vs venue −8.12 |
| 5 | blocker | GT ledger | Pre-reset 45 trips are not round trips | 0 paper_exit fills; 43/45 entry-tagged exits; −4.88 vs −115.33 |
| 6 | blocker | balance | Only E3 reconciles | E0 −5.91; E1 −10.07/−0.54; E2 −0.06 fees |
| 7 | blocker | balance | Four capital epochs, no capital ledger | 30300 nominal; 30086.14 discarded at resets; balance ≠ deposits + PnL |
| 8 | blocker | fees | Lifetime fee gap | venue 6.78 vs DB 6.18; 113 vs 103 buy legs |
| 9 | blocker | GT pairing | Entry fill closes an open row | entry-tagged exit; real exit becomes orphan |
| 10 | blocker | engine netting | Opposite fill closes any member's position | 7 netting pairs; 14 dropped exits; +7.58 |
| 11 | blocker | risk gate | Market order price=0 bypasses notional cap | 4 post-fill rejects; 14.35 USD unrecorded |
| 12 | blocker | report window | Reports in memory/20; restart loses history | bouncy −2.21; current ledger not rebuildable |
| 13 | major | attribution | Netting PnL credited to the wrong member | bouncy +10.09 / v10 −2.51 of +7.58 |
| 14 | major | strategy name | Pre-11:46 rows labelled momentum_scalper | 63 rows; 29 team entries; −26.88 mislabelled |
| 15 | major | ids | client_order_id not unique | 27 ids / 81 rows / max 8 |
| 16 | major | GT vs engine | GT cannot reconcile on any epoch | 45 trips vs 79 venue exits; −1.57 vs −97.80 |
| 17 | major | E0 | Missing sell-side close row | −26.88 vs −32.79; −1.81 re-seed at 15:45:49 |
| 18 | major | per-member | Stored-tag per-member PnL has wrong signs | bouncy +4.40 stored vs −3.54 venue; v10 −2.70 vs −0.19 |
| 19 | major | GT quanto | 100× under-scale | 45/45 trips; rebuild at 100 = −174.04 |
| 20 | major | GT fees | 3.85 bp applied to CFD sells | 22.9% over; 50/108 fills |
| 21 | major | GT mispair | 2/4 trips booked against next entry | −19.85 vs corrected −13.13 |
| 22 | major | GT rebuild | Pre-reset rows not reproducible from own fills | 46 vs 45 trips; open_positions truncated at reset |
| 23 | major | upstream | 15.98 pnl / 0.60 fees unexplained across epochs | per-epoch table above |
| 24 | major | GT pairing | Side-based open/close decision is the root | entry-tagged fills close; missing exit |

---

## 6. Corrected remediation gate (Round 3)

The Round-3 reviewers rejected the per-finding plans as the acceptance
contract. Use this staged gate instead. Every assertion must be red on HEAD
(or an explicit canary) and green only when the identities hold.

### P0 — Fixtures and epoch contract (no code changes)

- Freeze the local-time boundaries from §2 in one helper; do not mix
  E0–E3 with A–D names.
- Assert the venue identity for all four snapshots first:
  `cash + locked = seed + realized − fees`.
- Fixtures: copies of `/tmp/pulse-audit/trades-paper.db`, the
  `paper-state.json.bak-*`, `gt-ledger-current.db`, plus a synthetic
  two-member ledger. Live files read-only; never `immutable=1` while the
  engine writes.

### P1 — Engine venue ledger

- **Struct contract**: `PaperExitEvent` must carry `realized_pnl`,
  `entry_price`, `client_order_id`, `market_type`, `leverage`, `quanto`,
  populated before `tick()` erases the position
  (`PaperVenue.hpp:92-101`, `PaperVenue.cpp:892-930`). The original
  "already carries realized pnl" claim is false.
- **Unknown-PositionManager exit**: write exactly one trade row with the
  venue PnL/fee for each of the 18 known ids; do not create a PM position,
  do not duplicate, do not touch entry-row PnL.
- **No entry PnL**: every entry row `pnl=0`; every venue position
  contributes exactly one PnL-bearing row; on a synthetic E1 replay
  `Σpnl == −74.55` and `Σfees == 4.44`; on E3 `−7.70 / 0.42`.
- **close_ fee**: use `PaperCfdClose.fee`; a short close records 0.06, a
  long close 0.00; the two historical zero-fee rows become 0.06.
  The close-fee fix alone yields E1 3.96, not 4.44 — document the 0.48
  dependency on the missing-exit fix.
- **Fee formula**: every CFD buy fill `fees == 6.0 * filled_qty`
  (0.01 → 0.06), every sell fill 0.

### P2 — Risk gate and netting

- `reserveNotional` must take an explicit **reference price** for market
  orders (do not mutate `req.price`; keep the idempotency fingerprint
  stable). A zero/absent reference price must fail closed once the API
  exists; "price=0 must reject" is not a valid test today.
- A reduce-only close with a ref price different from the entry must not be
  rejected or shrunk; only the flip excess consumes budget.
- CFD cross-member: A long 0.01 + B sell 0.01 → two engine positions, zero
  entry PnL, two venue exits; empty client ids never match; same-member
  sequential entries stay two positions.
- Futures canary: `SellFillClosesPositionWithPnl` and the `close_`
  reduce-only path stay netting; the CFD hedge policy must not change
  futures quantity/leverage/PnL.
- Post-fill `openPosition` failure: adopt the venue leg via
  `syncPositionFromExchange` (idempotent by `exchange_position_id`); do not
  close inline under `m_restMutex` (deadlock).
- Cap/limit: four concurrent 0.01-lot XAUUSD legs fit the configured cap;
  the fifth is rejected pre-trade and leaves no venue leg.

### P3 — GT ledger

- Persist `exchange_position_id` on fills (a `position_links` table or a
  column) and an epoch; `get_trade_history` must expose
  `exchange_position_id`, `from_ms/to_ms`, and the **newest** page (fix the
  `rbegin` bug).
- Restart backfill on a copy: 15 epoch fills → 7 trips, gross −7.70,
  fees 0.42, net −8.12, open pp-276; `paper_exit_pp-262_1` must attribute
  to `xau_team:xau_bouncy`, not an orphan.
- **Entry never closes an open row**: sell entry, opposite entry, real exit
  → 0 trips until the exit; the real trip is −20.00/−0.06/−20.06; the new
  long is open. This is red today.
- **Member-scoped rebuild**: rebuilding bouncy must not consume supertrend's
  exits (live copy currently creates 9 spurious bouncy orphans); rebuild must
  be one transaction with a backup, and must not mutate fills except through
  a recorded repair.
- Partial exits: two 0.005 exits → two trips/orphans-free, no residual open
  row (the in-memory map pop currently orphans the second).
- Same-batch ordering: a futures batch delivered newest-first must still
  close (sort ascending inside `_ingest_fills`).
- Orphans: carry fees and expose an unallocated adjustment; `summary()` must
  not silently exclude them.
- Fee model: CFD buy 0.06, sell 0; an empty/unknown `market_type` must not
  hit the 3.85 bp estimate (currently 1.35386 at quanto=100).
- Replay: add a CFD replay test (0.01 lot, quanto 100) that produces 1 trip;
  current tests are futures-only.
- Brake isolation: a repaired/backfilled historical loss must not feed the
  live peak/kill; assert peak 1.83 unchanged, no false pause, and a genuine
  −20 daily loss still pauses.

### P4 — Capital epochs and reconciliation

- Capital events per leg (spot/futures/cfd), persisted atomically with the
  state and mirrored to JSONL/SQLite; no-SQLite build must work.
- Reset with a flat book: `cash = deposits + realized − fees`; reset with an
  open venue position is refused or carries explicit accounting; no locked
  margin vanishes.
- Legacy adjustments: A −5.91/0.00, B −10.07/−0.54, C 0.00/−0.06, D 0/0,
  appended once and idempotent. Startup integrity check is **WARN-only**
  (never refuses start/shutdown), tolerance ≤ 0.005.
- E0 acceptance: complete log −30.98 plus the explicit 15:45:49 re-seed
  −1.81 equals −32.79; do not assert `trades.pnl == realized` without it.
- Crash window: `kill -9` between a close and the 1 s snapshot must restore
  the venue position and record the exit once (idempotent key).

### P5 — CI/integration gates

- Enable `tests/integration` in CMake and add the target; run the GT suite
  with `python3 -m unittest` when `pytest` collection fails on
  `tests/test_client.py:24` (PySide6.QtTest).
- Add a no-SQLite compile/test job (`PULSE_ENABLE_SQLITE` defaults OFF).
- Update the 36-tool assertions (`test_mcp_server.cpp:103,186`), AGENTS.md
  and docs for any new RPC.
- Migration test: append columns after index 19, bump `user_version`, old
  binary still reads; historical rows byte-identical; rollback restores.

**Acceptance rule**: no finding closes unless its P-phase test is red on HEAD
(except canaries), green after the fix, and the account identity, fee
identity and GT bridge (`Σnet + unallocated = account net`) hold per epoch.

---

## 7. Red-first tests still missing (30)

These are the tests the Round-3 reviewers require; the existing suite is
green while all defects are present.

1. `tests/unit/execution/test_paper_venue.cpp::ExitEventCarriesRealizedPnlAndEntryPrice` — short SL exit: realized −3.77, entry 3621.38, fee 0.06, member `xau_team:bouncy:000001`, cfd, leverage 1000, quanto 100. Fails to compile today.
2. `test_paper_venue.cpp::ShortCloseChargesBuyLegCommission` — 0.01-lot short close charges 0.06.
3. `test_paper_venue.cpp::TwoExitsSamePositionPartialClose` — two 0.005 exits carry pro-rata fees and the remainder.
4. `tests/unit/control/test_engine_services.cpp::PaperExitUnknownPositionRecordsTheVenueRow` — inject an exit absent from PositionManager; exactly one trades row with venue PnL/fee, no PM mutation, no double count.
5. `test_engine_services.cpp::PaperExitDoesNotDoubleCountNettingEntry` — paper-cfd-205 → pp-206 case: one economic close, not two.
6. `test_engine_services.cpp::ClosePositionRecordsTheVenueFee` — short close fees 0.06, pnl from `PaperCfdClose`; live path stays 0.
7. `test_engine_services.cpp::TradeHistoryReturnsNewestPageAndTimeFilter` — 210 rows, limit 200 + from/to; newest rows returned; from_ms/to_ms exist.
8. `test_engine_services.cpp::TickPaperVenueWritesOneRowPerVenuePosition` — synthetic E1: 74 entries, 74 PnL rows, −74.55/4.44, cash 21.01.
9. `tests/unit/control/test_order_flow_executor.cpp::CrossMemberOppositeCfdFillKeepsBothPositions` — A buy + B sell: two positions, zero entry PnL, two venue exits.
10. `test_order_flow_executor.cpp::EmptyClientIdNeverNets` — two manual CFD opens with empty ids must not net.
11. `test_order_flow_executor.cpp::FuturesCloseStillNetsAfterHedgePolicy` — canary: futures sizing/leverage/PnL unchanged.
12. `test_order_flow_executor.cpp::PostFillOpenFailureAdoptsVenuePosition` — cap failure after venue fill → `syncPositionFromExchange` adopts; `get_positions` shows it; no venue-only leg.
13. `tests/unit/risk/test_position_manager.cpp::MarketOrderWithReferencePriceBindsNotionalCap` and `ReduceOnlyCloseNotShrunkByReferencePrice` — ref-price API; no price=0 rejection test.
14. `tests/unit/trade_recorder/test_trade_recorder.cpp::MigrateAppendsExchangePositionIdAndEpoch` — v0/v1 → v2, append-only, old binary reads.
15. `test_trade_recorder.cpp::DuplicateOrderIdIsAudited` — collision must ERROR/outbox, not only warn.
16. GT `tests/test_ledger.py::test_entry_never_closes_an_open_row` — sell entry + opposite entry + real exit: 0 trips until exit, then −20.00/−0.06/−20.06; new long open. Red today (the opposite entry books −25.00 immediately).
17. `test_ledger.py::test_rebuild_is_member_scoped` — bouncy rebuild must not consume supertrend exits; 0 spurious orphans (live copy: 9).
18. `test_ledger.py::test_rebuild_is_transactional_and_backs_up` — exception mid-replay leaves DB byte-identical.
19. `test_ledger.py::test_orphan_exit_fees_are_reported` — orphan carries fee and appears in an unallocated adjustment.
20. GT `tests/test_runner.py::test_exit_attribution_after_restart` — entry leaves the 20-report window; exit after restart must attribute via a persisted position link, not the bare team key. Red today (in-memory map popped).
21. `test_runner.py::test_same_batch_futures_entry_and_exit` — newest-first batch still yields one trip/zero orphans.
22. GT `tests/test_replay.py::test_cfd_replay_round_trip` — CFD 0.01 lot, quanto 100: 1 trip; currently futures-only.
23. `tests/test_team_risk.py::test_repaired_history_does_not_false_kill` — repaired history must not pause/kill; genuine −20 still does.
24. `test_ledger.py::test_unattributed_exits_are_separate_from_member_keys` — no fallback to `self.strategy.name`; explicit bucket.
25. `tests/unit/execution/test_paper_account.cpp::ClosedRoundTripIdentity` — cash == seed + realized − fees − locked within 1e-9 after a buy+sell.
26. `test_paper_account_store.cpp::LegacyV2StateRemainsReadableAndDowngradeSafe` — v2 state + new keys load; old binary cannot resurrect a pre-reset balance from `.bak`.
27. `test_mcp_server.cpp::ToolCountMatchesRegistry` — new RPC updates the 36-tool assertions and docs.
28. CI: enable `tests/integration`; no-SQLite job; GT unittest runner.
29. `get_trade_history`/backfill must not read `trades-paper.db` with `immutable=1` while the engine writes; use `mode=ro`, handle `SQLITE_BUSY`, assert no partial import.
30. Coverage gate: the 273 targeted ctest cases and 85 GT unittest cases currently exercise no dropped venue exit, close fee, cross-member netting, cross-member rebuild, or report-window backfill.

---

## 8. Reproduction and data

- Snapshot: `/tmp/pulse-audit/` (may be ephemeral). Source data:
  - `~/1_Code/09_pulseTrader/data/trades-paper.db` (+ `-wal`, `-shm`, dated `.bak-*`)
  - `~/1_Code/09_pulseTrader/data/paper-state.json` (+ 5 backups)
  - `~/1_Code/11_gold-trader-mind/data/pt_ledger-paper.db` (+ 3 backups)
  - `config/team.paper.json`, `trading.paper.toml`
  - `~/1_Code/09_pulseTrader/logs/paper/{app,risk,control,paper}.log`
- **Units**: `trades.timestamp` is UNIX **nanoseconds**; GT `fills.fill_time_ns`,
  `round_trips.opened_at_ns/closed_at_ns` are ns; JSON `time_ms` is ms.
- Make a consistent copy, never read a live WAL DB with `immutable=1`:

```bash
sqlite3 data/trades-paper.db ".backup /tmp/audit-trades.db"
sqlite3 ~/1_Code/11_gold-trader-mind/data/pt_ledger-paper.db \
        ".backup /tmp/audit-gt-ledger.db"
```

- Epoch sums:

```sql
SELECT CASE WHEN id<=63 THEN 'E0' WHEN id<=194 THEN 'E1'
            WHEN id<=196 THEN 'E2' ELSE 'E3' END ep,
       COUNT(*), ROUND(SUM(pnl),4), ROUND(SUM(fees),4)
FROM trades GROUP BY ep;
```

- Verification baselines (serial):
  - GT: `cd ~/1_Code/11_gold-trader-mind && env -u PYTHONPATH .venv/bin/python -m pytest -q` → 355 passed / 4 skipped (pytest collection can fail on `tests/test_client.py` in a clean env; use `python3 -m unittest` there).
  - PT: `ctest --test-dir build` → 1307/1307 serially (the parallel run has a known `WarmupSeeder` flake).
  - PT Qt: `ctest --test-dir build_qt_main` → 11/11.

---

## 9. Round 2/3 verdicts and unresolved gaps

- Round 1: 24/24 confirmed; some numeric corrections are reflected above.
- Round 2: all 24 assessed. Blockers are high/medium; required mitigations are
  the P1–P4 contracts.
- Round 3: **3/3 accept=false.** Reasons:
  - Several fixes rely on fields/events that do not exist (`PaperExitEvent`
    lacks realized PnL/entry/member; `get_trade_history` lacks
    `exchange_position_id`/time filter and returns the oldest page).
  - Several tests are unsatisfiable or green on HEAD (fee formula 100×,
    balance epoch signs, pre-reset −115.33 from fills, per-member sum
    −72.06 ≠ −74.55, strategy-name tautology).
  - Fixes are not composable: missing-exit + entry-PnL zeroing without
    accounting double-counts; close-fee-only leaves 0.48; disabling CFD
    netting needs a reference-price gate and higher concurrent notional;
    historical backfill can false-pause/kill the live brake.
- Live reachable gaps not yet tested: cross-member `rebuild_from_fills`
  contamination (9 spurious bouncy orphans on the live copy), cancelled
  reports, same-batch newest-first ordering, partial exits.
- No fixes were applied by the audit. The only code change in this turn is
  the PT Orders strategy filter (`aa2503b`), unrelated to accounting.

---

## 10. Open decisions for the fixing session

1. **Single source of truth**: adopt "venue close = one PnL row per venue
   position; engine trades = append-only audit; GT = venue report + explicit
   unallocated bucket + epoch". Confirm before coding.
2. **Netting policy**: make CFD/paper 1:1 with venue positions (no netting)
   vs keep netting. If 1:1, raise `maxPositionNotionalCfd` (per-market total)
   and re-check the 20 USD daily brake with 4 concurrent legs.
3. **History**: archive the old 45-trip/mispair ledgers and never present
   them as history, or attempt a repair from `trades-paper.db` after P1.
   Recommend archive + explicit unallocated adjustments.
4. **Capital events**: JSONL mirror + SQLite; no hand edits of
   `paper-state.json`; add a control RPC for seed/top-up/reset/withdraw.
5. **Report persistence**: persist the PaperVenue report ring (or a
   position-link journal) so a restart/ledger rebuild cannot lose fills.
6. **CI**: enable integration tests, no-SQLite job, GT unittest runner, and
   update the 36-tool/MCP assertions for any new method.

---

## 11. Audit evidence ledger

- **Command evidence**: GT pytest 355 passed / 4 skipped; PT ctest 1307/1307
  serial; PaperVenue/PaperAccount tests 34/34; Qt 11/11; every finding's SQL
  and file:line references are in the per-finding evidence in the audit
  output (972 KB, `tasks/wgmjjojuj.output`, not committed).
- **File evidence**: `app/runner.py`, `app/ledger.py`, `app/team/leader.py`,
  `src/control/EngineServices.cpp`, `src/control/OrderFlowExecutor.cpp`,
  `src/execution/PaperVenue.cpp`, `src/execution/PaperAccount.cpp`,
  `src/risk/PositionManager.cpp`, `trading.paper.toml`, GT ledger backups.
- **Unresolved**: Round 3 all accept=false; the remediation is not
  implementation-ready; no fix has been applied or verified.
- **Model evidence**: caller Claude Code (Workflow) / session model
  `deepseek-flash`; selected sub-agent model **opus**; decision = upgrade
  (high-risk financial reconciliation); downgrade check = pass.
- **Recursion evidence**: policy denied; depth 1; maxDepth 1; no nested
  launches.
- **Code changes**: none by the audit. This document is the record; the
  Orders filter `aa2503b` is a separate UI change.

---

*This is an engineering audit, not investment advice. All amounts are paper
USDT; no real-money order was placed by this work.*

---

## 12. Fixing-session log

### 2026-11-27 — P0 complete

- Frozen the E0..E3 contract in `tests/unit/control/reconciliation_epochs.hpp`
  (PT) and `tests/reconciliation_epochs.py` (GT). Boundaries convert civil time
  at a fixed +08:00 offset, so the contract also holds on the MSVC build, which
  ships no IANA tzdata.
- Committed read-only fixtures: `tests/fixtures/reconciliation/` (four
  `paper-state` snapshots plus a 90 KB trades-only trim, 210 rows, Σpnl −99.22,
  Σfees 6.18, `user_version` 1) and the GT ledger fixture in
  `11_gold-trader-mind/tests/fixtures/reconciliation/`. The trade rows are
  byte-identical to the 12:53 CST snapshot.
- Canaries: PT 6/6 under SQLite and 4 pass + 1 skip with
  `PULSE_ENABLE_SQLITE=OFF` (clean under `-Werror`); GT 7/7; GT suite
  362 passed / 4 skipped.
- §10 decisions confirmed: **venue truth** (§10.1) and **CFD 1:1 with venue
  positions plus a raised notional cap** (§10.2).

### 2026-11-27 — P1 complete (engine venue ledger)

- `PaperExitEvent` now carries `realized_pnl`, `entry_price`,
  `client_order_id`, `market_type`, `leverage` and `quanto`, populated in
  `closePositionLocked` before `tick()` erases the position.
- `tickPaperVenue` records exactly one PnL-bearing row per venue exit,
  including exits whose PositionManager position is missing (the previous
  `continue` lost 18 exits, −2.49 PnL and −0.48 fees). The row uses the venue's
  PnL, fee, leverage and quanto; a known-position mismatch is logged, and a
  failed mirror close no longer suppresses the venue row.
- `close_position` records `PaperCfdClose.fee` (and the venue realized PnL)
  instead of a hard-coded `fees = 0`; the live path still records 0.
- `recordCfdClose` / `recordExternalClose` take an optional
  `quanto_multiplier`; 0 keeps the registry fallback.
- Tests: `ExitEventCarriesRealizedPnlAndEntryPrice`,
  `ShortCloseChargesBuyLegCommission`, `LongCloseChargesNoCommission`,
  `TwoExitsSamePositionPartialClose`, `PaperExitUnknownPositionRecordsTheVenueRow`,
  `ClosePositionRecordsTheVenueFee`.

**Known transient state.** P1 is not sufficient on its own. Entry rows can
still carry netting PnL until P2 makes CFD 1:1 with the venue; running a
P1-only binary against live state would double-count those netting PnLs
against the newly recorded venue exits. The running paper engine must stay on
the pre-P1 binary until the P2 tests pass on a copy (§0.5).

### 2026-11-27 — P2 complete (CFD 1:1 with venue positions)

Done:

- `onOrderComplete` no longer nets CFD fills: a fill never closes another
  member's (or an earlier) CFD position. Futures and spot keep the historical
  netting — the `SellFillClosesPositionWithPnl` canary is unchanged.
- CFD entry rows now carry `pnl = 0`; the +7.58 netting PnL disappears at the
  source instead of being attributed to an entry.
- Reference price: `OrderRequest.reference_price` (excluded from the open_order
  fingerprint), `reserveNotional(..., reference_price)`, and a fail-closed
  rejection when a non-reduce-only market order has neither a price nor a
  reference. `EngineServices::referencePriceFor` resolves it from the paper
  venue mark or the live ticker; signal orders use `sig.price`; futures closes
  use the position's last mark. reduce_only stays exempt so a close is never
  trapped.
- Post-fill `openPosition` failure now adopts the venue leg through
  `syncPositionFromExchange` (idempotent by `exchange_position_id`), so a real
  venue position can no longer be invisible to the risk engine.
- `trading.paper.toml`: `maxPositionNotionalCfd` 8000 → 20000, and the comment
  corrected from "per-position" to "per-market total". The old value silently
  allowed only two concurrent 0.01-lot legs (§4.6); 20000 fits four plus one.
- Tests: `CrossMemberOppositeCfdFillKeepsBothPositions`,
  `EmptyClientIdCfdOpensDoNotNet`, `CfdEntryRowsCarryNoPnl`,
  `MarketOrderWithReferencePriceBindsNotionalCap`,
  `MarketOrderWithoutReferencePriceFailsClosed`,
  `ReduceOnlyCloseNotShrunkByReferencePrice`,
  `ReduceOnlyCloseWithoutReferenceStillApproves`,
  `PostFillOpenFailureAdoptsVenuePosition`,
  `FifthConcurrentCfdLegRejectedPreTrade`, and
  `E1ReplayReproducesVenueTotals` (74 entries + 74 exits, Σpnl −74.55,
  Σfees 4.44, cash 21.01 — fixture: `tests/fixtures/reconciliation/e1-legs.json`).
- Full PT suite: **1329/1329**.

The running paper engine remains on the pre-P1 binary until the operator
restarts it (§0.5); P1–P2 are now both green on a copy.
