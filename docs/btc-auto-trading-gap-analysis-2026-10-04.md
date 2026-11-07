# BTC Unattended Auto-Trading — Readiness Gap Analysis

> Date: 2026-10-04 · Branch: `headless` · Baseline: repository HEAD `7f0d072` + read-only live inspection of the VPS
> Method: a formation of 12 analysis agents cross-validating each other — 5 domain readers (strategy / risk and execution / market data connectivity / control-plane operations / research evidence)
> → 3 gap analysts (engineering readiness / strategy edge / unattended safety) → 3 adversarial verifiers (checking every claim back against the code)
> → 1 completeness critic. Conclusion: 19/19 safety-dimension claims confirmed; engineering 17 confirmed / 1 corrected; edge 11 confirmed / 1 uncertain.
> On the VPS side only read-only inspection was done (engine status / disk / file inventory); no changes were made.

## Yardstick (the definition of "done" used in this report)

**Unattended · real money · BTC_USDT futures · Gate mainnet · VPS deployment**: the whole chain from signal → automatic order → risk control → position management → monitoring and alerting must run without a human watching the screen. "It runs" and "it has a positive expected edge" are assessed separately.

## Conclusion

**"It can place orders automatically" is one config switch away; "we would dare let it run unattended with real money" is 8 blocker-level engineering gaps away (roughly 1–2 weeks of intensive fixes + 2–4 weeks of drills and pilot runs); "it is worth running" is an entire research cycle away — today 0 of the 6 running strategies has a fee-inclusive validation artifact, and the only strategy that was ever tested over a full cycle has been falsified.**

| Dimension | Readiness | In one sentence |
|---|---|---|
| Pipeline works | High (only deployment + the switch are missing) | The full chain signal→aggregation→risk→order→tracking→reconciliation is closed; 1045 tests green |
| Unattended safety | Low | All five fuses — stop-loss / circuit breaker / alerting / watchdog / kill switch — are either unwired or defective |
| Strategy edge | Zero evidence | Not one of the running strategies has a fee-inclusive backtest artifact; IronTrader, the only one validated over a full cycle, has been falsified |

⚠️ **Key caveat: the current VPS deployment has zero live mileage.** All 409 fills in trades.db predate this deployment (the old era, 2026-08-14 to 09-02); this deployment ran with `signal_only=true` the whole time, so only the "signal computation" branch was ever exercised — order placement, private-WS fill reports, reconciliation and the effect of halt on closing positions have never been triggered under a real order flow.

## 1. Current state (the foundation is solid)

- The 9-layer architecture is complete; 7 built-in strategies plus a Registry fallback; BTC_USDT has 6 enabled instances configured
  (momentum / orderbook / mean_reversion / supertrend / eth_scalper / ema_resonance) + 1 disabled (iron_trader)
- The signal→order path is closed: aggregator (threshold 0.6 / cooldown 30s) → auto_trade gate → direction gate → risk control → OrderFlowExecutor
- 32 control-plane methods (MCP / JSON-RPC / REPL share the same names and stay in sync); single-instance flock; position reconciliation at startup and every ~10s
- The M33 backtest engine + signal log + paper-replay toolchain; a mirrored Binance 7-year full-history dataset; a text rule document per strategy
- The recent WS reconnect fix `c37364b` (public channels fully re-subscribed by (channel, payload)) is committed but **not deployed**

The problem is not missing features; it is **last-mile wiring** and **the absence of a safety net**.

## 2. The 8 blockers (all must be fixed before flipping `signal_only=false`)

### B1. Live trading has no automatic exit / stop-loss path (the most lethal)

- After a futures entry fills, **no exchange-side stop-loss is placed at all**: `src/execution/OrderExecutor.cpp:183-215` (the futures body carries no price_sl/price_tp;
  only CFD has them, at `:238-248`)
- The strategy's Flat exit signal is dropped outright: `src/control/OrderFlowExecutor.cpp:216-219`
- `StopLossEngine` / `TakeProfitEngine` are dead code (the only references in the whole repository are in `tests/unit/risk/`);
  the `[risk.stop_loss]` and `[risk.take_profit]` configs are parsed but have no consumer on the execution side
- The Gate `price_orders` trigger exists only as a manual MCP tool (`EngineServices.cpp:957-1024`), and only supports closing the whole position (size must be 0)

→ Once a position is open, the only automatic exit is an "opposite entry signal". **If the engine dies or the feed stops, the position runs naked, and liquidation is the only backstop.**

Fix (days): first probe-verify the whole-position semantics of `price_orders` on BTC_USDT and whether it can wrongly hit a merged long/short position → after an entry fills,
automatically place a reduce-only trigger SL (still effective if the engine dies); then wire StopLoss/TakeProfitEngine (including trailing/max_hold) as the second layer inside the engine.

### B2. The daily-loss / drawdown circuit breaker is completely inert

- `DrawdownGuard.updateEquity / resetDaily` has no production caller (`src/risk/DrawdownGuard.cpp:47-104,150-166`;
  a repository-wide grep only hits tests) → the configured **daily loss of 2% / peak drawdown of 5% can never fire** (`trading.toml:384-385`)
- `OrderFlowExecutor.cpp:704` recordPnl only accumulates and never decides; in practice halt has a manual entry point only
- The data source already exists: the 60s heartbeat in `main.cpp` already reads futures balance.total; only the wiring is missing

Fix (hours): feed `updateEquity` from the heartbeat and call `resetDaily` on the UTC day rollover; define the trigger action explicitly (stop new orders + cancel working entry orders, optionally reduce exposure).

### B3. Zero alerting + zero watchdog

- The repository contains no outbound notification implementation whatsoever (no Telegram / webhook / email); `docs/OPERATIONAL_GUIDE.md:104` lists it as a P2 TODO;
  the AlertManager in `docs/architecture.md:147` is a documentation fiction
- The heartbeat only writes local logs (`apps/pulsetrader/main.cpp:1612-1628`); the systemd unit has `WatchdogUSec=infinity` and an empty `OnFailure=`
  (measured on the VPS), and `Restart=always/5s` only reacts to "process exit" — **a process that is "falsely alive" (running but stuck or zombied) cannot be detected by anyone**
- Hard disk deadline: engine.out measures **178MB/day**, and the VPS has 5.8G free ≈ **32 days to full** (a full disk means a silent shutdown)
- Precedent already exists: the A/B experiment idled on a dead feed for 13 days without anyone noticing

Fix (days): one outbound channel covering the event list (process exit/restart, halt, WS feed loss, order-rejection storms, daily-loss watermarks, distance to liquidation);
a systemd `OnFailure=` hook; an external dead-man's-switch (healthchecks.io or self-hosted).

### B4. The kill switch has serious flaws

- The halt check runs **before** the reduce_only exemption (`src/risk/RiskManager.cpp:50-61` vs `PositionManager.cpp:707-750`)
  → **after a halt even closing orders are rejected** (futures `close_position` passes through the same gate, `EngineServices.cpp:866-877`)
- halt does not cancel working orders (orders keep filling while trading is halted), its state is not persisted, and there is no `cancel_all` / `panic` method
  (`cancelAllOpenOrders` is only called by switch_direction)

Fix (days): exempt reduce_only / closing orders from halt; add an idempotent cancel_all_orders and panic (flatten + halt);
persist halt state and its reason + define startup recovery semantics (abnormal exit → halted by default + alert).

### B5. Private WS reconnect loses authentication + orders have no periodic reconciliation

- `c37364b` only fixed public channels: the reconnect replay goes through `buildResubscribeMsgs` **without auth**
  (`src/exchange/GateWsClient.cpp:486-498` vs `:160-175`); `ChannelEntry` has no private flag (`GateWsChannels.hpp:147-151`)
- `OrderTracker.m_wsSubscribed` is set once and never reset (`OrderTracker.cpp:83-91`); `reconcileAll` only fires on control-plane queries /
  direction switches, never periodically from the main loop

→ **Fill reports can be silently lost**, and the position / in-flight order view drifts for a long time with nobody noticing.

Fix (days): add the auth block on reconnect + force a full reconciliation after every successful reconnect + low-frequency order reconciliation every 10-30s in the main loop + regression tests.

### B6. A frozen market feed cannot be detected + `kline.closed` is always true

- The WS client does not record the last receive time, has no read timeout and never pings → **a half-open dead TCP connection never triggers a reconnect** (`GateWsClient.cpp:528-630`)
- The strategies / aggregator never check data timestamps (`SignalAggregator.cpp:30-63,96-154` ignores signal.timestamp entirely;
  OrderBookScalper polls the same snapshot every 500ms without checking `OrderBook.timestamp`)
- `MarketFeed.cpp:517` **hardcodes `closed=true`** on every push (the exchange actually pushes forming candles; a captured frame sample contains `"w":false`)
  → a strategy may auto-trade on an unclosed candle treated as final, and systematically diverges from the backtest (which uses complete closed candles)

Fix (days): per-channel last_msg_ts + a watchdog (force a reconnect after N seconds of silence); feed-staleness gates in the strategies / risk layer;
parse the `w` field to fix the closed semantics; have KlineBuffer replace by open_time instead of accumulating.

### B7. Order placement is not idempotent

- The REST layer **automatically retries** POST timeouts / 5xx (`src/exchange/GateRestClient.cpp:284-318`), while the automatic signal path
  **does not generate a client_order_id** (`OrderFlowExecutor.cpp:129-208`; the manual / grid paths do, `OrderExecutor.cpp:280-284`)
- → When a POST was already accepted by the exchange but the response timed out, the retry means **a duplicate position** (double exposure), and both orders consume risk budget

Fix (hours): generate a unique client_order_id on the automatic path → look the order up by that key before resending to deduplicate; or disable automatic retries for order-placement POSTs.

### B8. The signal → fill → performance attribution chain is broken

- `buildRequestFromSignal` does not write a client_order_id → `recordCompletedTrade` gets an empty strategy_id and falls back to
  **"the first enabled instance for that symbol"** (momentum_scalper for BTC), `OrderFlowExecutor.cpp:794-799,821-840`
- Impact: `get_strategy_performance`, the AI tuning input and any future per-strategy circuit breaker all rest on dirty data

Fix (days): plumb the attribution fields through end to end + add unit tests that pin down the multi-strategy-same-symbol scenario.

## 3. Operations / deployment / hygiene gaps (not blockers, but must be cleared before going live)

| # | Item | Current state (measured 2026-10-04) |
|---|---|---|
| O1 | VPS binary is 28 days stale | Stuck at the 09-06 build, **missing `c37364b` and every commit after 09-06 10:05**; the engine is inactive+disabled, and recovery needs sudo |
| O2 | Go-live config has not been trimmed | The VPS config has 4 symbols and 17 enabled instances; the notional budgets 12000/6500 were calibrated against the old grid peaks → trim to BTC only + re-scale to the pilot capital |
| O3 | Secret / permission hygiene | Both `.env` files are 664 (world-readable, and contain OPENAI_API_KEY); the key permissions / IP whitelists were never verified on site; the control port 127.0.0.1:8081 has **zero authentication** (any local process can place orders) |
| O4 | No disaster recovery for data | trades.db is 971MB on the VPS / 487MB locally, with no backup / archive / quick_check; signals and ticker_ticks have no retention policy |
| O5 | Two engines with no leader | The local machine and the VPS each hold mainnet keys; sync imports every futures position from the exchange with no filter → if both engines run, they write "external close" records into each other's ledger and corrupt it |
| O6 | Runtime state is not persisted | halt / peak / daily loss / auto_trade / active_market are all in-memory; set_strategy_param changes and ParamChangeLog are also in-memory → a restart silently reverts them |
| O7 | Reconciliation blind spots | A **partial reduction of a futures position is invisible** (`PositionManager.cpp:70-92` lowers only _sync, not the fill-tracked quantity) → the engine overstates exposure and phantom positions consume budget; external closes are valued at the last mark price with fees=0 |
| O8 | Pre-trade risk checks are not wired to live trading | The leverage / margin checks (`RiskManager.cpp:97-140`) are only called from unit tests, and leverage is passed straight to the exchange; liquidation distance is stored but never evaluated (`risk_types.hpp:100`); funding fees never enter PnL or risk control |
| O9 | Parameter units are mismatched | The parameter boundary table is calibrated in "base currency" while contracts are counted in number of contracts; the per-strategy stop_loss_pct/take_profit_pct have no consumer on the execution side (the operator and the AI believe a stop-loss is set, while nothing actually happens) |
| O10 | Main-loop blocking surface | Single-threaded with a global rest_mutex (shared by order placement / position sync / heartbeat / grid), and REST timeouts of 10s × 3 retries → worst case ~40s blocked while holding the lock |
| O11 | Aggregator residual lock-up | The weighted sum is only cleared after an emission and never decays with time → under mixed flows an opposite residual can keep the consensus emission "gated shut" for a long time, and there is no "time-since-last-consensus" health metric (`docs/ab-weight-trial.md` §2 says so itself) |
| O12 | Dirty data in the trades table | Records of impossible magnitude exist (e.g. a single BTC trade with pnl=+44,920, about 28× the physical upper bound) pending audit |
| — | Upgrade chain | The deploy script triggers a self-healing upgrade with `kill -9`, designed for "zero-position signal mode"; there is no procedure for upgrading with open positions / rollback / drills; the root unit and runbook are not in the repository |

**One correction**: the first draft claimed "the VPS AI heartbeat is tuning parameters on fake performance" — adversarial verification **refuted** this: the actual variable name is
`OPENAI_API_KEY` while `[ai].apiKey=from_env:AI_API_KEY` resolves to empty, and engine.out has printed
`AI pipeline disabled (no API key or interval=0)` on every start since 09-04 — **the AI was never powered on**. The broken attribution chain is real, but its impact is limited to statistics / auditing.

## 4. Strategy edge: the real deep water (this is what decides "is it worth it")

- **0 of the 6 running BTC strategies has a fee-inclusive backtest artifact**; `results/` contains only iron_* outputs
- IronTrader, the only strategy ever tested over a full cycle, has been falsified and stays disabled:
  - 480-combination sweep: futures 240/240 all negative, spot 240/240 all negative
  - 15m top combo over 3 weeks +12.93, of which **79% came from a single trade** (largest_win 10.18) → extended to 3 months **−15.22**
  - 1h tuned over 3 months +21.28 = 8 trades, with the top single trade +27.28 (>100% of net profit), not credible
  - 5m was the worst (3 weeks −20.06): wider stops plus double fees
- **The backtest engine cannot certify "survivability with real money"**: no funding fees / margin / liquidation / slippage, and fills happen instantly at the signal candle's close
  (`docs/backtest.md:66-71` states these limits); its semantics also diverge from live trading (the backtest ignores same-direction entries and flips, live stacks positions;
  live `closed` is always true) → **even a good number cannot be extrapolated**, fidelity must be upgraded first
- **Only two survivable candidates remain, and neither qualifies**:
  1. **Funding-rate harvesting**: the direction is real on 7 years of Binance full-history data (an independent recomputation of the sign model at 0.5bp gives +68%/+56% over 7 years,
     the same order of magnitude as the recorded +61%; a 5bp event gate yields only +24.8%; raising fees from 0.02% to 0.04% turns +68% into +48%; 2026 has gone roughly a year without a window),
     but the repository states three inconsistent versions of the methodology, there is no reproduction script, and **every execution prerequisite is missing** (FundingWatch is observation-only and undeployed; there is no paired two-leg execution,
     no spot↔futures transfer, no margin/liquidation monitoring for the short leg; the evidence comes from Binance while execution happens on Gate)
  2. **Grid**: the only automated strategy with live PnL, but the evidence is thin and negative (ETH v1 live ≈ −33.6 USD; SNDK had exactly one fill the whole time);
     the M27 engine-native version is implemented but has zero fee-inclusive statistics
- **AI weight tuning**: the pilot never started (`docs/ab-weight-trial.md` §7: not a single vote was ever cast), and the broken attribution chain pollutes its input — starting the AI heartbeat
  before attribution is fixed is a net negative
- Engineering vs research: **100% of the engineering gaps are "deterministic fixes" (week-level); "can it make money" is 100% decided by research,
  which needs weeks-to-months of calendar time at the fastest, and the worst-case conclusion is NO-GO**

## 5. The most likely outcome when nobody is watching

A naked position (B1) → losses accumulate silently (B2: no circuit breaker exists, and liquidation distance is not monitored) → **liquidation is the only backstop**, with no alert before or after (B3).
On restart the halt / daily-loss state is reset (**fail-open**: trading resumes automatically once the breaker has been cleared), or under signal_only the engine silently idles
(**fail-silent**) — both directions are silent. Also: the habit of closing positions manually in the App compounds the reconciliation blind spots (O7): engine exposure is overstated,
phantom positions consume budget, and a subsequent "close" may actually open a new position in the opposite direction.

## 6. Recommended path (ordered by dependency)

### Phase 0 — today, no code, no risk (30 minutes)

1. Check the mainnet **balance / positions / open orders** in the Gate App or web UI — if a naked position was left when trading stopped on 09-27, close it manually first
   (there is no backstop SL; this is the number-one risk today)
2. `chmod 600` both `.env` files; verify the key permissions (withdrawal disabled, IP whitelist set, re-create the keys if necessary)
3. Keep the VPS inactive+disabled until the drills pass

### Phase 1 — fix the 8 blockers (about 1–2 weeks)

Pick the low-hanging fruit first: B2 breaker wiring (hours), B7 idempotency (hours); then schedule the day-sized items: B1 dual-track stop-loss, B4 kill switch,
B5 private WS, B6 staleness/closed, B8 attribution, B3 alerting/watchdog. In parallel: trim the go-live config to BTC only, and persist a per-instance auto_trade seed.

### Phase 2 — deploy + drills (about 1 week)

Rebuild the bundle (`c37364b` + all fixes) → deploy (still signal_only) → **set up logrotate before the long run** (the 32-day countdown to a full disk
is real) → systemd watchdog + one alert channel → three drills: reconnect (4 symbols + private fill reports),
`kill -9` crash recovery (halt preserved / orphan orders swept / state restored), and per-event alert injection.
**An end-to-end drill (one minimum-size order on testnet or mainnet driving "signal→order→fill report→reconciliation") is the go-live gate.**

### Phase 3 — validation (2–4 weeks, two tracks in parallel)

- **Engineering track**: accumulate paper running under signal_only → 1–2 weeks of one strategy with minimal capital (acceptance: zero missed alerts, zero orphan orders,
  correct attribution, stop-loss / circuit breaker actually observed firing) → only then discuss scaling up
- **Research track**: fee-inclusive out-of-sample backtests of the 6 kline strategies (thresholds fixed up front: net OOS positive + largest single win < 15% of net profit +
  a stable parameter plateau) → backtest fidelity upgrade (funding / margin / slippage / closed semantics) → paper run of ≥300 rounds, then freeze the contract and adjudicate
- **Parallel track**: for funding-rate harvesting, first land a reproduction script and paper-capture real windows, then discuss the two-leg executor

**Red line: keep `signal_only=true` until the research side says GO. "It runs" and "it has an edge" are two independent gates; neither may mask the other.**

## Appendix: verification record (excerpts from the adversarial review)

- **All 19/19 safety-dimension claims confirmed**, 0 refuted
- **eng-ops: 17 confirmed / 1 refuted** (the AI_API_KEY claim; see the correction above)
- **edge: 11 confirmed / 1 uncertain** (the "60% engineering / 40% research" effort split is a subjective estimate and was not included in the conclusions;
  "every engineering gap has a deterministic fix" holds after cross-checking the other dimensions)
- Important omissions added by the verifiers (already merged above): automatic CFD entries also have no stop-loss; runtime parameter changes are not persisted; the aggregator residual locks up;
  trades.db has no disaster recovery; two engines have no leader; the control plane has zero authentication; the main loop's rest_mutex blocking surface; portfolio correlation shares one budget;
  the 32-day disk deadline; the local 1m database is actually ~80% coverage (including whole gaps) and cannot be called "full history"
