# XAUUSD Sub-Agent × Engine Signal Board (SignalBoard) Design and Implementation Guide

> **Version 1.1 · 2026-08-17 · Status: implemented (731 tests green, engine live in signal_only mode)**

## Implementation revision log

- **§3.7 place_trigger_order changed to "open_order with attached SL/TP"**: during implementation we found that the price_orders described in the 08-16 memo are **futures-only** (the memo itself marks them "not applicable to spot/CFD"). The correct mechanism for CFD is the `price_sl`/`price_tp` carried in the MT5 order body itself (already recorded in docs/CFD_TRADFI.md, and reserved in the original comment of OrderExecutor buildOrderBody) → it landed as: `OrderRequest.sl_price/tp_price` (optional) + the CFD branch of buildOrderBody filling them + `open_order` parameters `sl_price`/`tp_price` (REPL `--sl/--tp`, MCP schema), explicitly rejected for non-cfd. The effect equals the original design goal (exchange-native stop-loss, effective even if the engine and sub-agent are both down), with one fewer new method. The scenario of separately placing a trigger order to protect an existing position (rather than an entry order) is not supported for now; when needed, test the tradfi trigger semantics separately.
- **Gate location**: `signal_only` goes into the entry of `OrderFlowExecutor::onSignal` (after the Flat check, before the direction gate) rather than the main callback — so OrderFlowTest can test it directly, and manual placeOrder is naturally unaffected.
- **Indicator snapshot**: TradingSignal gains `indicators` (json, default `{}`), already filled in by four strategies (see the §3.3 table).
- **Baseline offset**: the implementation is based on commit `21f76ab` (M19.1); the main.cpp line numbers have shifted slightly from version 1.0 (strategy callback 919, aggregate callback 1058, EngineServices 1072), the structure is unchanged.

> **Version 1.0 · 2026-08-17**
> **Baseline**: pulseTrader `headless` branch, commit `1ced101` (another session on this machine is editing code, so run `git pull` to align before implementing; the line numbers in this document are based on the baseline above — if the code has moved on, trust the actual code)
> **Audience**: the Claude Code session responsible for the implementation. This document is a specification of "what to change and how"; it does not contain code you can paste wholesale; every line number / filename points at the baseline code, so just follow the pattern.
> **Companion file**: `xauusd-subagent-strategy.md` (the sub-agent rule baseline v1; §4 here is its v2, to be written back after landing)

---

## 1. Background and goals

**The conflict**: the engine's 3 XAUUSD CFD strategies (momentum/mean_reversion/supertrend) place orders automatically according to configuration, and they know nothing about the LLM sub-agent's order decisions → two-headed trading, closing each other's positions, and breaching the "at most 1 position" constraint.

**The approach**: **decouple** signal generation from order execution. The strategies keep running but only publish signals to the factor board (SignalBoard); the sub-agent reads it periodically (`get_signals`) as a decision factor and is the **only order placer** (it uses the manual path `open_order`/`close_position`, with all risk control retained). The user keeps adding "strategy tools", each tool = a new factor, and they all flow into the factor board.

**Goals**:

- **G1** The engine can be configured as signal-only: strategy signals do not trigger orders
- **G2** Each strategy's real-time signal + indicator snapshot is published to the factor board
- **G3** `get_signals` is readable through all three channels: JSON-RPC / MCP / REPL
- **G4** The aggregator's consensus signal is published as an independent factor as well (independent of signal_only; both modes publish it, so post-mortems can audit it)
- **G5** The manual order path is **not** affected by signal_only (this is the sub-agent's channel)
- **G6** Extension point: when new factor tools are added later, the sub-agent-side interface does not change at all

---

## 2. Architecture overview

```
Strategy thread × N (polling every 500ms)
   │  TradingSignal (new indicators field)
   ├──────────────────────────► SignalAggregator ──aggregate signal──► (main.cpp output callback)
   │                                    │                          │
   │ raw signal + indicator snapshot     │                          ▼
   └──► SignalBoard (factor board) ◄── publishAggregate ──────────  OrderFlowExecutor.onSignal
            │  latest per strategy_id + aggregate slot             │ signal_only=true → return immediately
            │                                                      │ signal_only=false → risk control→order (current behaviour)
            ▼  get_signals = EngineServices::signals()
   control plane (JSON-RPC 8081 / MCP stdio / REPL)
            ▲
            │  read every round (only factors fresher than ≤120s are valid)
        Sub-agent (LLM)── its own rules (§4) + factors → open_order/close_position (manual path, bypasses the aggregator)
```

Key points:

- The manual path (`openOrder`) and the signal path (`onSignal`) are **different entry points** in `OrderFlowExecutor`, so the signal_only gate only applies to the signal path and naturally does not affect sub-agent orders (verify the function names during implementation, see §7)
- The factor board is an **in-memory structure**, cleared on engine restart (the sub-agent filters by freshness, so no persistence is needed)

---

## 3. Engine-side implementation specification

### 3.1 Configuration: `signal_only`

- **Location**: `StrategyConfig` (`src/core/config.hpp:288`, currently holding `signal_aggregator_threshold` / `signal_cooldown_sec`)
- **Field**: `bool signal_only = false;` · TOML: `signal_only = false` under the `[strategy]` section
- **loader**: read with the same pattern as the other fields (find_or); **validator**: no constraint (boolean)
- **Semantics**: `true` = strategy signals only go to the factor board and the aggregate output does not place orders; manual `open_order`/`close_position` are unaffected

### 3.2 New component SignalBoard

- **Files**: `src/strategy/signal/SignalBoard.hpp` + `.cpp` (same directory as `SignalAggregator`); add to `src/strategy/CMakeLists.txt`
- **Data structure**:

```cpp
namespace pulse::strategy
{
struct FactorEntry
{
    std::string source;      // = strategy_id, e.g. "momentum_scalper_XAUUSD"; the aggregate slot is "aggregate"
    Symbol symbol;           // "XAUUSD"
    MarketType market_type;  // Cfd / Futures / Spot
    SignalType type;         // Buy / Sell / Flat
    double confidence;       // 0..1
    Price price;             // signal reference price
    std::int64_t ts_ms;      // engine clock, milliseconds
    std::string reason;      // human-readable reason
    nlohmann::json indicators; // strategy indicator snapshot (§3.3), may be {}
};
}
```

- **Class interface**:

```cpp
class SignalBoard
{
  public:
    explicit SignalBoard(double aggregateThreshold);
    void publish(const TradingSignal &sig);          // overwrite-save the latest entry keyed by strategy_id
    void publishAggregate(const TradingSignal &sig); // aggregate slot (carries the threshold)
    [[nodiscard]] nlohmann::json snapshot() const;               // full snapshot
    [[nodiscard]] nlohmann::json snapshot(MarketType mt) const;  // filter by market
    [[nodiscard]] std::uint64_t entryCount() const;

  private:
    mutable std::shared_mutex m_mutex;
    std::map<std::string, FactorEntry> m_latest;  // key = strategy_id
    std::optional<FactorEntry> m_aggregate;
    double m_aggregateThreshold;
};
```

- **Thread safety**: `publish` comes from several strategy threads (once per 500ms, low frequency) and `snapshot` from the control thread → `shared_mutex` is enough, and the hot-path overhead is negligible
- **The JSON structure returned by `snapshot()`** (`ts_str` follows `[control] display_timezone` and reuses the existing formatting in `src/core/TimeUtil.hpp`, consistent with the other methods):

```json
{
  "signals": [
    {
      "source": "momentum_scalper_XAUUSD",
      "symbol": "XAUUSD", "market_type": "cfd",
      "type": "buy", "confidence": 0.72, "price": 4401.2,
      "ts_ms": 1786943000000, "ts_str": "2026-08-17T13:03:20+00:00",
      "reason": "EMA9 crossed above EMA21",
      "indicators": { "ema_fast": 4400.1, "ema_slow": 4399.7 }
    }
  ],
  "aggregate": {
    "source": "aggregate", "symbol": "XAUUSD", "market_type": "cfd",
    "type": "flat", "confidence": 0.42, "threshold": 0.6,
    "ts_ms": 1786943000000, "ts_str": "...", "reason": "...", "indicators": {}
  },
  "updated_at_ms": 1786943000000
}
```

### 3.3 Strategy indicator snapshots

- **Change `TradingSignal`** (`src/strategy/signal_types.hpp:49`): add the field `nlohmann::json indicators;`, default `{}` (add default initialisation in the constructor). The aggregator and existing tests are unaffected (empty JSON); add a default-construction assertion test
  - Alternative (not recommended unless the implementation session has a strong reason): leave TradingSignal alone and instead add a `getLastIndicators(id)` interface to StrategyManager. The change surface is larger
- **Each strategy stuffs its already-computed indicators into `indicators` before calling `emitSignal`**. The v1 principle: **publish only values that already exist, add no new computation** (new computation is left to the Phase 4 factor tools):

| Strategy | Suggested `indicators` fields (use the actual variable names inside the strategy) |
|---|---|
| MomentumScalper | `ema_fast`, `ema_slow`, `ema_diff`, `last_close` |
| MeanReversionScalper | `bb_mid`, `bb_upper`, `bb_lower`, `rsi14` (if already computed), `z_score` (if already computed) |
| SuperTrendScalper | `supertrend_value`, `supertrend_dir` (1/-1), `last_close` |
| OrderBookScalper | `best_bid`, `best_ask`, `imbalance` (futures side; not used by the sub-agent yet, but kept) |

- If a strategy computes no indicators at all, v1 allows `{}`; Phase 4 fills them in uniformly

### 3.4 main.cpp wiring (two edits + one construction)

1. **Create the board** (before `apps/pulsetrader/main.cpp:914`):

```cpp
auto board = std::make_shared<pulse::strategy::SignalBoard>(
    cfg.strategy.signal_aggregator_threshold);
```

2. **Strategy signal callback** (main.cpp:914-919) becomes:

```cpp
strategy_mgr.setSignalCallback(
    [&aggregator, board](const pulse::strategy::TradingSignal &sig)
    {
        board->publish(sig);        // raw signal + indicator snapshot go to the factor board
        aggregator.addSignal(sig);
    });
```

3. **Aggregate output callback** (main.cpp:1049-1053) becomes:

```cpp
aggregator.setOutputCallback(
    [&order_flow, board](const pulse::strategy::TradingSignal &sig)
    {
        board->publishAggregate(sig);   // published in both modes so post-mortems can audit
        order_flow.onSignal(sig);       // the gate lives inside OrderFlowExecutor
    });
```

4. **EngineServices construction** (around main.cpp:1063) passes in the board (see §3.6-1)

### 3.5 OrderFlowExecutor: the signal_only gate

- The `OrderFlowExecutor` constructor **already receives `cfg.strategy`** (main.cpp:972-989, first parameter) → store `bool m_signalOnly`
- At the entry of `onSignal()`: `if (m_signalOnly) { return; }` (optionally: a low-frequency debug log saying "signal published but not executed")
- **The gate goes here rather than in the main callback** because: (a) `OrderFlowTest` can test it directly; (b) it covers every signal entry uniformly; (c) manual `openOrder` is a different entry point and is naturally unaffected
- Add a `[[nodiscard]] bool signalOnly() const;` accessor for test assertions
- ⚠️ Please verify whether `onSignal` has necessary side effects other than order placement (such as statistics counters); if so, keep the side effects and short-circuit only the order-placement part

### 3.6 The `get_signals` control-plane method (four channels)

1. **EngineServices** (`src/control/EngineServices.hpp`):
   - Add a constructor parameter `strategy::SignalBoard &signalBoard` (or shared_ptr; a reference is recommended for consistency with the other components)
   - New query method: `[[nodiscard]] nlohmann::json signals() const;` → `m_signalBoard.snapshot()`
   - Update the construction sites in main.cpp and `EngineServicesTest`
2. **JsonRpcServer** (`src/control/JsonRpcServer.cpp`; the registry follows the `reg["get_market"]` at lines 371-447):

```cpp
reg["get_signals"] = [&services](const nlohmann::json &)
{
    return services.signals();
};
```

3. **McpServer::toolDefinitions()** (the `add` pattern at `src/control/McpServer.cpp:56`):

```cpp
add("get_signals",
    "Latest per-strategy signals + indicator snapshots + aggregate consensus "
    "from the signal board. Entries older than ~120s should be treated as "
    "stale by consumers.",
    nlohmann::json::object(), {});
```

> ⚠️ M14 lesson: `required` in inputSchema must be an **array**; a nested boolean `"required": true` is forbidden (there is a comment at McpServer.cpp:40-41)

4. **CommandParser**: new REPL command `signals` (no arguments; follow the `market` command pattern at line 218) → `ParsedCommand{ "get_signals", {} }`; render source/type/confidence/age with the existing `renderTable`
5. **ControlClient**: if it has a typed method list, add `signals()` (verify during implementation)

### 3.7 The `place_trigger_order` method (exchange-side protective stop-loss, a P0 must-have)

- **Purpose**: after the sub-agent opens a position, immediately place a stop-loss trigger order on the Gate exchange side — so the stop-loss still fires even if both the engine and the sub-agent are down (solving the naked-position risk of "the stop-loss only exists inside the sub-agent's polling loop")
- **Implementation basis**: `0_note/gate-trading/gate-futures-trigger-order-tp-sl-notes_2026-08-16.md` (the price_orders interface + the sign_req function, including 7 pitfalls; must-read before implementing)
- **Method**: `EngineServices::placeTriggerOrder(const nlohmann::json &params)`
  - params: `{ symbol, side("buy"|"sell"), quantity, trigger_price, market_type, trigger_kind("stop_loss"|"take_profit"|"trigger") }`
  - Returns: the order response. ⚠️ tradfi order placement **does not echo the order id** → reuse the M17 `matchCfdOrderId` pattern to parse the id out of the open-orders list (see `src/execution/OrderExecutor.cpp`)
  - Channels: JSON-RPC + MCP tool (these two are the minimum requirement; REPL optional)
- If the scope conflicts it can be split out as Phase 2b, but it **must be finished before the sub-agent opens its first live position**

### 3.8 Test checklist (target: all 687 existing tests green + ~15-20 new)

| Test file | Cases |
|---|---|
| `SignalBoardTest` (new) | publish overwrites the latest per strategy_id; snapshot structure; the aggregate slot; empty-board snapshot; concurrent publish+snapshot smoke test (shared_mutex) |
| Existing `signal_types` test | TradingSignal defaults `indicators == {}` |
| `OrderFlowTest` | `SignalOnlySkipsExecution` (signal_only=true → no order/no reservation after onSignal); `SignalOnlyManualOrderStillWorks` (manual openOrder unaffected) |
| config loader/validator tests | `signal_only` true/false/defaults to false |
| `JsonRpcServerTest` | get_signals is registered and returns the board JSON |
| `McpServerTest` | tools/list contains get_signals; the full call chain returns normally |
| `CommandParserTest` | parsing `"signals"` |
| `EngineServicesTest` | `signals()` is wired to the injected board |

### 3.9 Documentation sync (do it in passing during the implementation session)

- `AGENTS.md`: method list 16→17 (+place_trigger_order makes it 18)
- `OPERATIONAL_GUIDE.md`: add get_signals / REPL `signals` to the control-plane section
- `README.md` (if it lists methods): sync
- `.claude/project-memory.md`: add an M18 subsection (a summary of this design landing) + update the test count (687→N)
- `trading.toml`: add `signal_only = false` under `[strategy]` (with a comment; default off, keeping the current behaviour)

### 3.10 Acceptance criteria (Definition of Done)

- [ ] All tests green (687 + new)
- [ ] Starting with `signal_only=true`: the XAUUSD strategies keep running, `get_signals` keeps returning data, and zero new orders are observed for ≥10 minutes
- [ ] `signal_only=false` regression: behaviour matches the current production setup (covered by tests)
- [ ] Manual `open_order` still works under signal_only=true
- [ ] `get_signals` appears in MCP `tools/list`; REPL `signals` produces output
- [ ] Commit style follows the convention (feat/fix/docs prefix, test count in the commit message)

---

## 4. Sub-agent v2 decision model (factor-based decisions)

> This is the v2 upgrade of `xauusd-subagent-strategy.md`; write it back into that file once landed. Factor sources: the SignalBoard (`get_signals`) + the sub-agent's own computation (`get_market` klines/ticker). **Only factors fresher than ≤120s are counted.**

**Gate layer (if any one fails → do nothing)**:

| Factor | Condition |
|---|---|
| session | ∈ {london, ny} (the first version may relax this to weekdays 10:00–21:00 UTC; ⚠️ trading sessions / rollover / weekend times are subject to docs/CFD_TRADFI.md and actual measurements) |
| volatility_regime | ≠ hot (ATR14(1m) ≤ 3× the median of the last 20 bars; computed by the sub-agent itself until Phase 4 lands) |
| cooldown | no new position within 2 candles after a stop-loss / consecutive losses; stand aside when the sub-agent and the engine aggregate signal disagree on direction |
| daily risk | realised today > -8 USD, consecutive losses < 2, fewer than 4 round trips today (account basis 37~45 USD) |
| position | the sub-agent holds at most 1 CFD position; with a position open it only does exit management |

**Direction filter (mandatory)**:

- `ema_separation` points the same way and |separation| > 0.3×ATR (do not open during a glued phase; the sub-agent computes this from klines itself until Phase 4 lands)
- Enhancement (any one): the engine aggregate signal points the same way, or ≥2 strategy signals point the same way and each is fresh enough

**Trigger (≥1)**:

- RSI14 extreme (<35 long / >65 short, matching the direction)
- Price hugging support/resistance: within 0.5×ATR of a swing high/low without breaking it (confirmed by ticker)
- Breakout: a 1m close breaks the 20-bar range high/low (confirmed by ticker to avoid false breakouts)

**Exit**:

| Rule | Trigger | Action |
|---|---|---|
| Hard stop-loss | -5 USD | close (the exchange-side trigger order is the backstop, §3.7) |
| Take-profit | +8~10 USD | take the profit |
| Factor reversal | aggregate / ema_separation flips | close early |
| Time stop | held >60 minutes and unrealised profit < 1 USD | exit flat or at a small loss |
| Rollover | ≥15 minutes before the daily maintenance window | flatten (to avoid swap and gaps) |

**Execution and bookkeeping**:

- Open: `open_order` (market_type=cfd, type=market, 0.01) — **immediately** after opening, place the exchange-side protective stop-loss (`place_trigger_order`)
- Close: `close_position` (position_id comes from get_positions)
- State persistence: `0_note/gate-trading/gold/joey-Z170I-PRO-GAMING/state/xauusd-agent-state.json` (date / realised today / consecutive losses / cooldown deadline / position snapshot) — handovers and restarts lose nothing
- Per-trade post-mortem: `0_note/gate-trading/gold/joey-Z170I-PRO-GAMING/review/xauusd-review-YYYYMMDD-HHMMSS.md`; aggregate metrics go into `xauusd-stats.md` (win rate / expectancy / MAE / MFE / spread samples); weekly gate: 3 red days or an expectancy < 0 over 20 trades → pause and revise the rules
- Handover protocol: a report must include position state + stop-loss plan + last check time; after recovery **the first action is to check position health, then open new positions**

### §4.1 M22 revision (2026-08-18, live lesson from round 130→132: RSI 64.66 chased a long breakout near overbought, a 2.83 TR spike punched through the SL, slippage 0.77, realised -5.53, account 10.62→5.09 and trading stopped outright)

**New rules (already synced into the task brief)**:

1. **Overbought/oversold hedge against breakout filtering**: RSI > 65 → do not chase a long breakout / RSI < 35 → do not chase a short breakout; an exception requires both the enhancement and direction to be stronger (⚖️ threshold |separation| ≥ 0.5×ATR and a fresh same-direction aggregate)
2. **Spike risk pre-check**: before entry, check the maxTR of the last 20 bars; maxTR > SL distance (5 points) → record the expected slippage; maxTR > 2×SL distance (10 points) → skip that entry
3. **Capital thickness gate (gate 6)**: available − new-position margin < 2× the stop-loss amount (-10 USD) → do not trade

**New code (engine M22, 759 tests green)**:

4. **External close audit trail**: when sync_positions clears a phantom position it synthesises an ExecutionReport (`ext_close_` prefix) + writes trades.db + logs, completing the audit chain (covering exchange-side SL/TP fills and manual closes in the user's App — f36d8cd only covered engine-initiated closes)
5. **Minimum available funds after stop gate**: `risk.minAvailableAfterStopUsd` (off by default); opening a CFD position with an SL requires available − margin − stop-loss amount ≥ that value, otherwise it is rejected with 3009 InsufficientFreeMargin

---

## 5. Phase 4 factor tool specification (the extension point)

> Not in Phase 2's scope, but the interface is reserved as described here so that "adding a tool = adding a factor, with zero changes to the sub-agent interface".

**Interface draft** (implemented later; suggested file `src/strategy/signal/FactorEngine.hpp`):

```cpp
class FactorSource
{
  public:
    virtual ~FactorSource() = default;
    virtual std::string id() const = 0;      // "factor:volatility_regime" etc.
    virtual void onKline(const Kline &k, MarketType mt) = 0;   // or onTicker
    virtual void publish(SignalBoard &board) = 0;              // writes a FactorEntry(source = id())
};
// FactorEngine: holds SignalBoard&, registers FactorSources, subscribes to MarketFeed events and dispatches
```

**First batch of tool specifications**:

| Tool id | Input | Logic | Output | Frequency | Priority |
|---|---|---|---|---|---|
| `factor:volatility_regime` | 1m klines | ATR14 vs the median of the last 20 bars → calm/normal/hot | indicators: {atr, median, regime} | every close | High (the §4 gate depends on it) |
| `factor:ema_separation` | 1m klines | (EMA9−EMA21)/ATR14 | {ema9, ema21, separation} | every close | High (the §4 direction filter depends on it) |
| `factor:sr_levels` | 1m klines | swing highs/lows with N=3 over the last 100 bars; nearest support/resistance and the distances | {support, resistance, dist_s, dist_r} | every close | High |
| `factor:spread` | CFD ticker | rolling median of ask−bid (parseCfdTicker already has bid_price/ask_price, MarketFeed.cpp:524) | {spread, median_60s} | every second | Medium (cost samples) |
| `factor:session` | static table | UTC time → asia/london/ny/rollover/closed | {session} | every minute | High (the session table must be verified by measurement) |
| `factor:ticker_momentum` | ticker | price change over the last 60s / ATR14 | {delta, delta_atr} | every second | Medium (entry confirmation) |

- Each tool = one FactorSource registration + one SignalBoard entry + 2~3 tests
- Tools are read-only and never place orders; orders always go through the sub-agent only

---

## 6. Transition and deployment

**Phase 1 (immediately, doable from any session, pure runtime operation, no code change)**:

- Pause the 3 XAUUSD strategies (they are **still running live** right now): MCP `pause_strategy` ×3 or REPL `pause <id>`, with ids:
  - `momentum_scalper_XAUUSD` / `mean_reversion_scalper_XAUUSD` / `supertrend_scalper_XAUUSD`
- ⚠️ A runtime pause **is lost on restart** (main.cpp:1080 restores cfd strategies automatically at startup according to `active_market`) — this is only a transitional safety net

**Phase 2**: implement per §3 → rebuild → test → commit → documentation sync (§3.9)

**Phase 3**:

1. Set `signal_only = true` in `trading.toml` → restart the engine (after the restart the XAUUSD strategies resume automatically, but under signal-only they only publish signals)
2. Accept per §3.10: observe ≥10 minutes with zero new orders, data in get_signals, and the manual path usable
3. Put sub-agent v2 (§4) live; the first live position may only be opened once `place_trigger_order` is ready
4. Write `xauusd-subagent-strategy.md` back to v2

**Phase 4**: add the factor tools one by one per §5 (each batch as its own commit)

**Rollback**: `signal_only = false` + restart = fully back to the current state; removing the components is clean (the new files are self-contained and do not intrude on existing paths)

---

## 7. Open items the implementation session must verify

1. The names of the **already-computed indicator variables** inside each strategy (the §3.3 table lands on these; v1 publishes only what already exists)
2. Does `StrategyBase::emitSignal` also emit `Flat` signals? (This affects factor-board coverage; if it only emits Buy/Sell, the sub-agent can rely on the 120s freshness filter and no change is needed)
3. The function name of the manual entry point of `OrderFlowExecutor` and its boundary with `onSignal` (the basis for placing the gate in §3.5; `src/control/OrderFlowExecutor.cpp`)
4. The exact location of the `JsonRpcServer` registry and how the closures capture (`src/control/JsonRpcServer.cpp:371-447`, the `reg[...]` pattern)
5. The `McpServer` dispatch mechanism (tool name → method forwarding? Just add one following the existing tools; `src/control/McpServer.cpp:56` onwards)
6. Whether `ControlClient` has a typed method list that needs syncing
7. Updating the construction site of `EngineServicesTest` (`src/control/` test directory)
8. Gate TradFi XAUUSD **trading sessions / daily maintenance window / rollover times** (`docs/CFD_TRADFI.md`; the §4 session factor and time stop depend on it; if unsure, measure and record)
9. Sync `.claude/project-memory.md` when committing (M18 subsection + test count)
10. If other sessions have committed during implementation, run `git pull` before starting work, and take the line numbers from the latest code

---

## 8. Reference index

| File | Purpose |
|---|---|
| `0_note/gate-trading/gate-futures-trigger-order-tp-sl-notes_2026-08-16.md` | price_orders + sign_req implementation (must-read for §3.7) |
| `0_note/gate-trading/gold/joey-Z170I-PRO-GAMING/strategy/xauusd-subagent-strategy.md` | Sub-agent rule baseline v1 (§4 here is its v2) |
| repo `docs/CFD_TRADFI.md` | CFD API investigation, trading sessions |
| repo `.claude/project-memory.md` | Control-plane method list, M14-M17 history, OrderFlowTest regression checklist |
| repo `OPERATIONAL_GUIDE.md` | Control-plane documentation (the §3.9 sync target) |
| repo `src/strategy/signal_types.hpp` / `signal/SignalAggregator.{hpp,cpp}` | Signal types and the aggregator (the mount points of this design) |
