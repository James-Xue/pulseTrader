# pulseTrader Architecture Analysis Report

> Analysis Date: 2026-06-20
> Scope: 9-layer architecture, ~70 source files
> Status: 3/6 high-priority issues fixed, 3 pending

## Completed Fixes

| # | Issue | Commit | Description |
|---|---|---|---|
| 1 | PnL=0.0, drawdown protection ineffective | `c857e21` | `close_position` returns `optional<double>`, main.cpp accumulates and passes to DrawdownGuard |
| 2 | AI parameter tuning disconnected | `786e9f8` | `StrategyManager.all_params()` collects real params, AiPipeline loops and writes to all strategies |
| 3 | `std::stod` crashes WS thread | `7c052cf` | Added `safe_parse_double()` (from_chars), replaced 34 call sites + 10 new tests |

## Overview

| Category | Count | Representative Issues |
|---|---|---|
| 🔴 Correctness/Safety | 6 | PnL=0, AI disconnection, stod crash, TOCTOU race |
| 🟡 Performance/Maintainability | 12 | Connection not reused, full snapshot copy, code duplication |
| 🟢 Code Quality | 9 | Dead code, const_cast, deprecated API |

**Top three priorities to address first:**
1. Connect PnL and AI feedback loops — the core value of the trading system, currently broken
2. Add exception protection to all `std::stod` calls — a one-line fix that prevents crashes
3. RiskManager TOCTOU — add an atomic "reserve" operation to prevent position over-allocation

---

## 🔴 High Priority — Affects Correctness / Data Safety

### 1. PnL is always 0.0, drawdown protection is effectively useless

- **Location**: `apps/pulsetrader/main.cpp`, order completion callback lambda
- **Symptom**: `pnl` is hardcoded to `0.0`, `DrawdownGuard::record_pnl()` never receives real profit/loss
- **Consequence**: `maxDailyDrawdown` limit is never triggered, drawdown protection does not work

### 2. AI parameter tuning is completely disconnected

- **Location**: `apps/pulsetrader/main.cpp` + `src/heartbeat/heartbeat_scheduler.cpp`
- **Symptom**: The `shared_params` written by `HeartbeatScheduler` and the params read by each strategy are not the same memory
- **Consequence**: Parameter adjustments computed by ParamAdvisor are never received by strategies. The entire AI → strategy feedback loop is broken

### 3. `std::stod` runs unprotected on WS event thread

- **Location**: `src/market/market_feed.cpp`, `src/execution/order_tracker.cpp`, multiple locations
- **Symptom**: `std::stod()` parses price/quantity fields returned by the exchange without try/catch
- **Consequence**: When Gate.io returns anomalous strings (empty string, "N/A"), the `std::invalid_argument` exception kills the WS event thread, causing market data interruption or program crash

### 4. RiskManager has a TOCTOU race condition

- **Location**: `src/risk/risk_manager.cpp` — `evaluate_order()`
- **Symptom**: Sequentially calls `can_open_position()` → `portfolio_summary()` → `symbol_notional()`, each independently acquiring/releasing a shared_lock
- **Consequence**: Two strategy threads can pass the check simultaneously between two lock acquisitions, each placing an order, resulting in position over-allocation
- **Fix direction**: Requires an atomic "check-and-reserve" operation

### 5. OrderTracker completion callback executes under write lock

- **Location**: `src/execution/order_tracker.cpp` — when order state transitions to terminal
- **Symptom**: Callback invokes `position_mgr.open_position()` and `drawdown_guard.record_pnl()`, and performs spdlog I/O while holding the lock
- **Consequence**: Lock ordering coupling risk + performance issue from I/O while holding lock

### 6. ProxyTunnel: 300 lines of code crammed into `gate_ws_client.cpp`

- **Location**: `src/exchange/gate_ws_client.cpp` anonymous namespace
- **Symptom**: A complete TCP listener + HTTP CONNECT negotiation + bidirectional relay network component with no separate header file
- **Consequence**: Cannot be tested in isolation, complex lifecycle management (detached thread hazard, though M13 shutdown fix added join)

---

## 🟡 Medium Priority — Affects Maintainability / Performance

### 7. REST requests do not reuse connections

- **Location**: `src/exchange/gate_rest_client.cpp` — `do_request()`
- **Symptom**: Each call does `curl_easy_init()` → request → `curl_easy_cleanup()`, paying the TCP+TLS handshake cost every time
- **Impact**: Acceptable for current call frequency, but will become a bottleneck for high-frequency order placement

### 8. REST retry does not refresh signature

- **Location**: `src/exchange/gate_rest_client.cpp` — `request()`
- **Symptom**: Signature (timestamp + HMAC) is computed once outside the retry loop, retries may span several seconds
- **Impact**: Gate.io time window is ~60s, in extreme cases the signature will expire

### 9. WebUI DashboardState full snapshot copy every 200ms

- **Location**: `src/webui/dashboard_state.cpp` — `poll_loop()`
- **Symptom**: Deep copies the entire `DashboardSnapshot` every 200ms (orderbook 40 levels + 100 K-lines + positions + orders + reports)
- **Impact**: Intense heap allocation, could be optimized with COW or diff push

### 10. WebUI can only display one market

- **Location**: `apps/pulsetrader/main.cpp`
- **Symptom**: `auto& ui_feed = spot_feed ? *spot_feed : *futures_feed` — prefers spot, otherwise futures
- **Impact**: Cannot display data from both markets simultaneously

### 11. `completed_reports_` grows without bound

- **Location**: `src/execution/order_tracker.cpp`
- **Symptom**: Completed reports are never cleaned up, `recent_reports()` O(n log n) sort becomes progressively slower
- **Impact**: Memory grows continuously over long runs

### 12. Three instances of duplicated utility code

- **Location**: `src/ai/news_feed.cpp`, `src/ai/twitter_feed.cpp`, `src/ai/ai_client.cpp`
- **Duplicated content**: `parse_iso8601`, `curl_write_callback`, `ensure_curl_init`
- **Additional issue**: `parse_iso8601` uses `mktime()`, which is affected by local timezone/DST, introducing a timezone bug

### 13. TwitterFeed URL encoding is incomplete

- **Location**: `src/ai/twitter_feed.cpp` — `url_encode()`
- **Symptom**: Only encodes spaces as `%20`, does not handle `#`, `&`, `=` and other special characters
- **Impact**: Query URLs are malformed when keywords contain `#bitcoin` etc.

### 14. SignalAggregator executes the full callback chain under mutex

- **Location**: `src/strategy/signal_aggregator.cpp` (if it exists) or the aggregator in `strategy_manager.cpp`
- **Symptom**: `add_signal()` holds the lock while calling `evaluate_and_emit()` → `output_callback_()`, which triggers risk assessment + synchronous REST order placement
- **Impact**: The entire chain blocks signal submissions from other strategy threads

### 15. HeartbeatScheduler `io_context` thread polls every 100ms

- **Location**: `src/heartbeat/heartbeat_scheduler.cpp`
- **Symptom**: `run_for(100ms)` wakes up every 100ms even during idle periods between 5-minute intervals
- **Fix direction**: Use `io_ctx.run()` + `io_ctx.stop()` for on-demand blocking/waking

### 16. 10 strategy parameters updated non-atomically

- **Location**: `src/ai/param_advisor.cpp` — `apply()`
- **Symptom**: Writes `atomic<double>` one by one, strategy threads may read mid-update
- **Impact**: A single evaluation cycle may use inconsistent stop-loss/take-profit/position parameters

### 17. Kline `closed` field is hardcoded to `true`

- **Location**: `src/market/market_feed.cpp`
- **Symptom**: `kline.closed = true` — cannot distinguish between completed K-lines and in-progress K-lines
- **Impact**: May mislead strategy logic that depends on K-line completion status

### 18. MomentumScalper has no local cooldown

- **Location**: `src/strategy/scalping/momentum_scalper.cpp` (if it exists)
- **Symptom**: The other two strategies have `last_signal_time_ms_` local cooldown, MomentumScalper relies solely on the aggregator's global cooldown
- **Impact**: In fast-oscillating markets, may emit a signal on every K-line

---

## 🟢 Low Priority — Code Quality / Future Expansion

### 19. main.cpp: 950 lines of manual wiring

- **Location**: `apps/pulsetrader/main.cpp`
- **Symptom**: Composition root is entirely hand-written, no DI container or builder
- **Impact**: Easy to miss initialization/shutdown ordering when adding layers or components

### 20. `const_cast` hack

- **Location**: `apps/pulsetrader/main.cpp`
- **Symptom**: `const_cast<pulse::webui::WsServer&>(ws_ref).push_snapshot(snap)` — `ws_server()` returns a const reference but `push_snapshot()` is not const
- **Fix direction**: Make `push_snapshot` const (internally mutable) or have `ws_server()` return non-const

### 21. OrderBookManager's resubscribe callback is dead code

- **Location**: `src/market/orderbook_manager.cpp` — `set_resubscribe_callback()`
- **Symptom**: Exists but is never called; on sequence number anomalies the system silently uses potentially stale orderbook data

### 22. Heartbeat event types defined but never used

- **Location**: `src/heartbeat/heartbeat_events.hpp`
- **Symptom**: `OnBeat`, `OnAnalysisDone`, `OnParamUpdate`, `HeartbeatEvent` variant are defined but never constructed/emitted/consumed
- **Nature**: Scaffolding for a future event bus

### 23. `needs_json_ping()` is dead code

- **Location**: `src/exchange/endpoint_router.hpp`
- **Symptom**: Method exists but is never called

### 24. WebUI static files read from disk synchronously on event loop thread

- **Location**: `src/webui/web_server.cpp` — `serve_static()`
- **Symptom**: `std::ifstream` reads files synchronously on the uWS event loop thread
- **Impact**: Disk latency blocks all HTTP/WS processing

### 25. `sha512_hex` uses deprecated OpenSSL API

- **Location**: `src/exchange/gate_auth.cpp`
- **Symptom**: `SHA512()` and `HMAC()` trigger compilation warnings on OpenSSL 3.x
- **Fix direction**: Migrate to EVP interfaces

### 26. `curl_global_init` scattered across three files

- **Location**: `ai_client.cpp`, `news_feed.cpp`, `twitter_feed.cpp`
- **Symptom**: Each uses its own `std::once_flag` to call `curl_global_init()`
- **Fix direction**: Consolidate into a single initialization point (e.g., `core/curl_init.hpp`)

### 27. `curl_global_cleanup` is never called

- **Symptom**: Relies on OS reclaiming resources
- **Impact**: Tools like valgrind will report leaks; if pulseTrader is embedded as a library, there is a real leak

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    apps/pulsetrader/main.cpp              │
│                    (Composition Root, Manual Wiring)      │
└──────┬──────┬──────┬──────┬──────┬──────┬──────┬────────┘
       │      │      │      │      │      │      │
   ┌───▼──┐┌──▼──┐┌──▼──┐┌─▼──┐┌──▼──┐┌──▼──┐┌──▼──┐
   │L1    ││L2   ││L3   ││L4  ││L5   ││L6   ││L9   │
   │Exch  ││Log  ││Mkt  ││AI  ││HB   ││Strat││WebUI│
   │      ││     ││     ││    ││     ││     ││     │
   │REST  ││spd  ││Feed ││Pipe││Sched││Mgr  ││uWS  │
   │WS ×2 ││log  ││OBook││Cli ││Task ││Aggr ││Dash │
   │Proxy ││     ││Ticker││News││Queue││×3   ││     │
   └──┬───┘└─────┘└──┬──┘└─┬──┘└──┬──┘└──┬──┘└──┬──┘
      │              │     │      │      │      │
      │         ┌────▼─────▼──────▼──────▼──────▼───┐
      │         │          L7 Risk                   │
      │         │  PosMgr | Drawdown | RateLimit     │
      │         │  StopLoss | TakeProfit             │
      │         └────────────┬───────────────────────┘
      │                      │
      │         ┌────────────▼───────────────────────┐
      │         │          L8 Execution               │
      │         │  OrderExec | OrderTracker | Report  │
      │         └────────────────────────────────────┘
      │
   ┌──▼──────────────┐    ┌─────────────────────┐
   │  Gate.io API     │    │ Optional             │
   │  REST + WS       │    │ L-SQLite TradeRec    │
   │  spot + futures  │    │                      │
   └─────────────────┘    └─────────────────────┘
```

## Thread Inventory

| Thread | Owner | Responsibility |
|---|---|---|
| Main thread | `main()` | 200ms polling, waits for SIGINT |
| Spot WS I/O | `GateWsClient` | Receives market data + order events |
| Futures WS I/O | `GateWsClient` | Receives market data + order events |
| Strategy thread(s) | `StrategyManager` | One polling thread per strategy |
| DashboardState poll | `DashboardState` | 50ms tiered polling |
| uWS event loop | `WebServer` | HTTP + WS handling |
| io_context | `HeartbeatScheduler` | Timer triggers |
| TaskQueue worker | `TaskQueue` | AI pipeline execution |

Typical configuration: approximately **8 threads**.

## Data Flow (Signal → Order → Position)

```
Strategy thread
  → emit_signal(TradingSignal)
    → SignalAggregator.add_signal()  [mutex]
      → evaluate_and_emit()
        → output_callback_()         [still holding lock!]
          → RiskManager.evaluate_order()  [shared_lock ×3, TOCTOU]
            → OrderExecutor.place_order()  [sync REST POST]
              → OrderTracker.track_order()  [unique_lock, WS sub]
                → [async] on_order_update()  [WS I/O thread]
                  → completion_callback_()   [unique_lock]
                    → PositionManager.open_position()
                    → DrawdownGuard.record_pnl(0.0)  ← the problem
```

## Signal Aggregation Mechanism

```
Strategy A ──signal──┐
Strategy B ──signal──┤→ SignalAggregator (per-symbol weighted accumulation)
Strategy C ──signal──┘    │
                           ├─ normalize dominant confidence
                           ├─ threshold check
                           ├─ cooldown check (global, independent of strategy-local cooldown)
                           └─ emit consolidated signal → RiskManager
```

## Discussion Items

- [x] #1 PnL calculation approach — `close_position` returns `std::optional<double>` (PnL implemented), main.cpp accumulates and passes to `drawdown_guard.record_pnl()` ✅
- [x] #2 AI feedback loop — `StrategyManager.all_params()` collects real params pointers from each strategy, `HeartbeatScheduler` + `AiPipeline::run()` changed to `vector<StrategyParams*>`, ParamAdvisor writes to all strategies ✅
- [x] #3 `std::stod` → `safe_parse_double()` — 34 replacements + 10 new tests, WS event thread no longer crashes on anomalous strings ✅
- [x] #4 TOCTOU: `PositionManager::reserve_notional()` atomic reservation mode, single unique_lock replaces 3 independent shared_locks, `pending_reservations_` prevents double-spending, `open_position()` auto-consumes reservations ✅
- [x] #5 Callbacks moved out from under lock: "collect under lock, execute outside lock" pattern, `completion_callback_` called after unique_lock release, `set_completion_callback()` protected with lock ✅
- [x] #6 ProxyTunnel extracted into standalone `proxy_tunnel.hpp/.cpp`, fixed detached thread use-after-free + relay registration race, removed 58 lines of dead code ✅
