# pulseTrader — Project Memory

> Last updated: 2026-08-16
> File size: 23862 chars / 25000 chars. Must recalculate and sync this line after updating this file.
> Historical details migrated to `project-memory-archive.md`

## Overview

- **Project**: pulseTrader — C++20 high-frequency scalping framework
- **Repository**: https://github.com/James-Xue/pulseTrader (public, GPL 3.0)
- **Exchange**: Gate.io (REST + WebSocket), single-exchange focus
- **Namespace**: `pulse::` · **Build**: CMake + vcpkg

## Architecture (9 Layers, All ✅)

| L1 Exchange | L2 Logging | L3 Market Data | L4 AI Analysis | L5 Heartbeat |
|---|---|---|---|---|
| L6 Strategy | L7 Risk Mgmt | L8 Execution | L9 Control Plane | — |

- Hot path (L1→L3→L6→L7→L8) vs AI background (L4→L5), bridged via `std::atomic`
- Control plane (L9, `headless` branch, replaces WebUI): JSON-RPC socket 127.0.0.1:8081 + embedded/remote REPL + stdio MCP server
- Proxy: REST via `CURLOPT_PROXY`; WS via `ProxyTunnel` class
- Credentials: `.env` (`GATE_API_KEY`/`GATE_API_SECRET`), gitignored

## Dependencies

- Core: nlohmann-json, spdlog, fmt, curl, openssl, asio, websocketpp, gtest, toml11
- Optional: sqlitecpp (`-DPULSE_ENABLE_SQLITE=ON`)
- Vendored: websocketpp in `third_party/` (uWebSockets/uSockets removed with the WebUI)
- SQLiteCpp GCC 15 fix: build with `-DCMAKE_CXX_FLAGS="-include cstdint"`

## Current State (M13 Done, 2026-06-21)

### Test Summary
- **669 tests all green** (CTest, `headless` branch): control plane (CommandParser, JsonRpcServer, McpServer, OrderFlowTest incl. M15 direction-gate + M16 maker-first tests, EngineServicesTest incl. switch tests, ControlClient) + core/config/logger/exchange/market/execution/risk/strategy/AI/heartbeat/trade_recorder suites
- M15 additions: gate rejects inactive market, switch allows cfd + rejects futures, reduce_only exemption, signal skip, cancel sweep, switchDirection (unconfigured fails / unknown rejected / noop / to-spot), openOrder defaults to active market, market() feed selection, evaluateCfdOrder (7101/7102), parseCfdDetail/validateOrder/mergeFrom, buildOrderBody CFD, cfd endpoint paths, REPL switch, parseCfdTicker/parseCfdKline
- M16 (2026-08-16) additions: maker-first order flow — 14 OrderFlowTest (best bid/ask post-only pricing, sweep cancel+fallback, partial-fill remainder, exchange-reject no-chase, rate-limit/direction-switch rejection, cancel-race) + 5 config validator + 3 config loader = 22 new
- OrderFlowTest 2026-08-14 regressions: OnSignalModifiedOrderIsPlaced, OnSignalModifiedFailureReleasesReservation, FuturesQuantoKeepsFullContractQuantity, SellFillOpensShortWhenNoLong

### Milestones
- **M1–M5** ✅: Core pipeline → strategy → risk → AI → control plane → trading engine
- **M6** ✅: TOML config (`--config trading.toml`, `from_env:` syntax)
- **M7** ✅: SQLite trade recorder (17-col schema, 4 queries)
- **M8** ✅: Futures config foundation (MarketType/MarginMode enums, 7xxx errors)
- **M9** ✅: EndpointRouter + WS ping/pong fix
- **M10** ✅: Futures Market Data
  - `Ticker`: mark_price, index_price, funding_rate fields
  - `SymbolInfo`: quanto_multiplier, leverage_max/min, maintenance_rate, funding_interval, order_size_min/max, market_type
  - `SymbolRegistry`: MarketType param, `parse_futures_contract()`, futures validate_order()
  - `MarketFeed`: MarketType param, EndpointRouter channel routing, dual-format JSON parsing
  - `EndpointRouter`: orders_path(), order_path(), leverage_path()
  - `GateRestClient`: post/cancel/get_futures_order()
  - 11 new tests
- **M11** ✅: Futures Risk / PnL
  - `Position`: market_type, leverage, margin_mode, margin_used, liquidation_price, quanto_multiplier
  - `PortfolioSummary`: total_margin_used, futures_position_count
  - `PositionManager`: leverage-aware PnL (`calculate_unrealized_pnl` with leverage/quanto), futures open_position overload, liquidation price estimation
  - `RiskManager`: evaluate_futures_order() — leverage + margin checks, first use of 7xxx error codes
  - 12 new tests
- **M12** ✅: Futures Execution + Dual-Market Wiring
  - `OrderRequest`: market_type, leverage, reduce_only, contract_size
  - `OrderExecutor`: MarketType param, futures order body (contract/size/tif), futures response parsing (int id, finish_as)
  - `OrderTracker`: MarketType param, EndpointRouter WS/REST routing
  - `TradingSignal`: market_type field, auto-set by emit_signal()
  - `main.cpp`: dual-market infrastructure (per-market REST/WS/Feed/Executor/Tracker), strategy→market routing
  - 7 new tests
- **M13** ✅: Testnet Support
  - `ExchangeConfig`: `bool testnet` field
  - `PULSE_NETWORK` env var: "mainnet" (default) / "testnet" switch
  - Testnet REST: `https://api-testnet.gateapi.io` (correct URL, not `fx-api-testnet`)
  - Testnet WS: uses mainnet `fx-ws.gateio.ws` (testnet WS unreachable from China; data identical)
  - TOML `[exchange] testnet = true` overrides REST URL automatically
  - Config validator: rejects spot strategies in testnet mode (futures-only)
  - `run.sh`: auto-loads `trading.toml` if no `--config` specified
  - SQLite: auto-creates `data/` directory for dbPath
  - `.env` structure: `GATE_MAINNET_*` / `GATE_TESTNET_*` key separation
  - 6 new tests (3 loader + 3 validator)

### Post-M13 Bugfixes (2026-06-20)
- **Ctrl+C graceful shutdown** — 3-layer fix in `gate_ws_client.cpp`:
  1. `GateWsClient::stop()`: `io_ctx_ptr->stop()` to force-stop asio event loop (unblocks `client.run()`)
  2. `ProxyTunnel` accept thread: `poll()` + 200ms timeout instead of blocking `accept()` (Linux `close()` can't interrupt blocking `accept()`)
  3. `ProxyTunnel` relay threads: no longer `detach()`; `stop()` closes sockets then `join()`s all relay threads
  4. `run_io_loop()`: explicit `tunnel->stop()` + `tunnel.reset()` before function return (correct cleanup order)
  5. `WsInternal`: added `io_ctx_ptr` field, set after `init_asio()`, cleared after `client.run()` returns
- **Strategy warmup diagnostics** — kline-driven strategies now log progress during cold start:
  - `MomentumScalper` / `MeanReversionScalper` `on_tick()`: logs "Waiting for kline data" every 30s when no klines exist (WS not connected)
  - `on_kline()`: logs "Warming up: X/N candles accumulated" every 30s when insufficient data for EMA/BB computation
  - New members: `last_warmup_log_ms_`, `last_no_data_log_ms_` in both `.hpp` headers
- **Aggregator threshold lowered** — `trading.toml` `signal_aggregator_threshold` from 0.7 → 0.6 to match single-strategy min_confidence, preventing valid signals from being silently dropped
- **OPERATIONAL_GUIDE.md updated** — §4.4 added strategy warmup period explanation (log examples + "wait at least 25 minutes"), §5.1 updated threshold tuning guidance, added Q7 "No orders placed after startup?" troubleshooting checklist

### Architecture Review Fixes (2026-06-20)
- **#1 PnL Wired to DrawdownGuard** (`c857e21`): `close_position()` returns `optional<double>` with realized PnL; main.cpp accumulates it and passes to `drawdown_guard.record_pnl()`. Drawdown protection is now active.
- **#2 AI Feedback Loop Wired** (`786e9f8`): `StrategyManager.all_params()` collects real params pointers from each strategy; `AiPipeline::run()` changed to accept `vector<StrategyParams*>&`; ParamAdvisor iterates and writes to all strategies' atomic params.
- **#3 stod Crash Prevention** (`7c052cf`): Added `safe_parse_double()` (based on `std::from_chars`, exception-free, locale-independent), replaced 34 `std::stod` calls + 10 new tests. Total tests 513.

### Testnet URL Auto-Switch Fix (2026-06-21)
- **Problem**: When `testnet=true`, only the REST URL switched to testnet; WS URL stayed on mainnet (private channel auth failures)
- **Solution**: `config_loader.cpp` adjusted load order — read `testnet` flag first → set URL defaults by network mode → then load URL fields with `find_or`
- **Result**: When `testnet=true`, REST/Spot WS/Futures WS all auto-switch to testnet addresses; explicit TOML URLs can override (China users falling back to mainnet WS)
- **Changes**: `config.hpp` added `pulse::url` namespace (6 URL constants); `config_loader.cpp` adjusted `parse_exchange()` load order; `main.cpp` removed old override block + banner displays all 3 URLs; `trading.toml` simplified (removed explicit URLs)
- **Added 4 tests**: `TestnetAutoSwitch`, `TestnetExplicitOverride`, `MainnetDefault`, `MainnetExplicit`
- **Laptop environment setup**: apt install libasio-dev 1.30.2 + libwebsocketpp-dev 0.8.2+git20250909 + libsqlitecpp-dev 3.3.3 + toml11 4.4.0 (~/.local)
- 532 tests all green (original 528 + 4 new tests)

### Testnet WS CloudFront TLS Incompatibility (2026-06-21, commit `0e61877`)
Testnet futures WS (`ws-testnet.gate.com`) sits behind CloudFront → websocketpp reports `Invalid HTTP status` (HTTP/2 negotiation). Fix: testnet WS uses the mainnet URL `wss://fx-ws.gateio.ws/v4/ws/usdt` (market data identical); REST stays on testnet. `trading.toml` lists the 3 URLs explicitly for override.

### System Heartbeat Logging (2026-06-21)
`FeedStats` atomic counters (ticker/orderbook/kline) + 60s heartbeat line: `[heartbeat] uptime 1h23m | futures 100 tick/s 10 kline/s 80 ob/s | ws spot=n/a futures=connected | strategies 3/3 running | positions 0 (notional 0.00 USDT) | account ...`. Hot path = 1 relaxed fetch_add.

### WebUI History (removed 2026-08-13 on `headless` branch)
Fully superseded by the control plane (JSON-RPC/REPL/MCP); all fixes are in git history.

### Account Balance (2026-06-21, still relevant)
- **AccountBalance struct**: total, available, unrealised_pnl, position_margin, order_margin, currency
- **REST parsing**: `GateRestClient::get_futures_account_balance()` — parses Gate.io futures account JSON (all values as strings → safe_parse_double)
- **Heartbeat log**: `... | account 1000.00 USDT (avail 950.00, pnl +2.50)`
- `Result<T>` is `std::variant<T, PulseError>` — use `ok()` / `value()` / `error()`, not `has_value()`

### Naming Convention Refactoring (2026-06-23)
- **Commit `cd8a4d5`**: Functions/methods snake_case → camelCase (~210 renames), member variables `trailing_underscore_` → `m_camelCase` prefix (~47 classes). 137 files, ±3271 lines. Exempt: `to_json`/`from_json` (ADL), TEST_F names, pure-data struct fields.
- **Commit `6309be0`**: File names renamed to match primary class name (PascalCase). 80 files (40 .hpp + 40 .cpp), e.g. `order_executor.hpp` → `OrderExecutor.hpp`. All `#include` paths and CMakeLists.txt updated. 7+ multi-type modules kept as-is (`config.hpp`, `types.hpp`, `risk_types.hpp`, etc.).
- **False positives fixed**: spdlog `set_level()`, websocketpp `get_payload()` — manually reverted.
- **Additional**: 3 snake_case helpers, 1 Yoda condition, 16 missing braces, 2 stale comments.
- AGENTS.md updated with new naming + file naming rules. 547 tests all green.

### Fast Ctrl+C Shutdown (2026-06-23, commit `0c1a7ed`)
3 blocking points fixed (20-90s → <1s): DashboardState REST (XFERINFO abort check + cancelRequests + 5s connect timeout), HeartbeatScheduler TaskQueue (`stop()` called explicitly), ProxyTunnel `remote_sock` hoisted to member so `stop()` can unblock `asio::connect()`. GateRestClient: custom move ops + `cancelRequests()`.

### vcpkg/Linux Build Compatibility (2026-06-23, commit `a812333`)
Remote vcpkg change (uSockets/uWebSockets + `get_io_service()`) reverted — Linux builds use vendored `third_party/` + `get_io_context()`.

### Next Steps (2026-08-16 pending)
- ⏳ SKHY 止损触发单 (170.5) existence check + risk handling — ≈1.3% from mark price, ≈-260 USDT if triggered
- ⏳ After gold market reopens (~08-17): clean leftover CFD trigger orders (buy@4295 id 17511143 / sell@4428 id 17471679) via `tools/test_gate_rest --tradfi-cleanup`
- ⏳ Maker-first verification: testnet first, then small live capital; watch logs "Maker-first attempt registered" / "Maker-first fallback"; consider `order_type = "maker_first"` on a futures instance (e.g. maker_timeout_ms 500)
- ⏳ Loopback port returning awselb responses (suspected Clash TUN hijack of loopback traffic) — can investigate separately
- ✅ #4 RiskManager TOCTOU — `PositionManager::reserve_notional()` atomic reservation mode, single unique_lock replacing 3 independent shared_locks. `RiskEvalResult` added `reservation_id`; `main.cpp` failure path calls `cancel_reservation()`, success path auto-consumes. 5 new tests.
- ✅ #5 OrderTracker Callback Under Write Lock — "collect inside lock, execute outside lock" pattern: `completion_callback_` in `process_order_update()` and `poll_order_status()` called after unique_lock is released. `set_completion_callback()` protected by lock. Added `test_simulate_ws_update()` / `test_try_shared_lock()` test interfaces. 3 new tests.
- ✅ #6 ProxyTunnel Extraction — 373 lines of network code extracted from `gate_ws_client.cpp` into `proxy_tunnel.hpp/.cpp`. Fixed 2 hidden bugs: (1) `handle_connection` thread changed from `.detach()` to joinable; (2) relay socket/thread registration merged into a single lock_guard scope. Removed 58 lines of dead code (SSL relay overloads). 7 new tests.
- ✅ #7 Risk-gate single evaluation (M14, 2026-08-14) — `onSignal` passes its eval into `placeOrder(req, eval)`; the 1-arg overload evaluates once. Kills the 3002 reject loop (Modified orders were rejected against their own reservation) and the reservation leak.
- ✅ #8 Futures quanto notional (M14) — `OrderRequest.quanto_multiplier`; `reserveNotional(qty, price, quanto)`; `SymbolRegistry` (919 futures contracts) fetched at startup in main.cpp and injected via `setSymbolRegistry()`; reserved notional clamped to budget (kills the 1-ULP overflow deadlock).
- ✅ #9 Symmetric fill tracking (M14) — fills close opposite-direction positions first, then open the remainder (SELL fill with no long now opens a tracked short). `m_reservations` stores `ReservationEntry{reservation_id, request}` so fills open positions with the correct market type/leverage/quanto.

### M14 + CFD Direction (2026-08-14) → M15 Dual-Direction Trading (2026-08-15, done)
- **Product pivot**: trading direction moved from BTC_USDT perpetual futures to **Gate.io TradFi/CFD gold (`XAUUSD`)** — see `docs/CFD_TRADFI.md` (full API survey + phased implementation plan) and OPERATIONAL_GUIDE §10.
- **Verified live**: CFD account registered (`mt5_uid 2017864`), XAUUSD ~4348 USD/oz, 1 lot = 100 oz, min 0.01 lot, leverage 20–500; API key **CFD permission works** (futures permission is a separate product — the 403 on `/futures/*` is expected and irrelevant to CFD).
- **M15 implemented (2026-08-15, 632 tests green)**: `MarketType::Cfd` + config (`active_market`, max_leverage→500); EndpointRouter `/api/v4/tradfi/*`; GateRestClient 11 TradFi methods; OrderExecutor static `buildOrderBody` (MT5 schema, live-verified); OrderTracker REST-poll mode; MarketFeed REST-poll loop (ticker 1s / klines 60s, backfill 500, dedupe); SymbolRegistry CFD branch + `mergeFrom`; `RiskManager::evaluateCfdOrder` (7101/7102, margin includes contract_volume); CFD close via `/tradfi/positions/{id}/close`; **direction switching**: `switch_direction` method/REPL `switch`/MCP tool (17 methods) — pauses old direction's strategies, cancels its open orders, positions stay open; gate in OrderFlowExecutor (`InactiveMarket 3008`, `reduce_only` exempt); `open_order` defaults to active direction; `get_market` market_type-aware; status shows `active_market`; heartbeat has cfd feed + USD balance.
- **Engine state**: rebuilt engine required before next run (kills stale mcp bridge first, see below). trading.toml has `active_market = "futures"` (default, safe) + a momentum XAUUSD CFD instance (enabled but paused at startup). **CFD never trades until `switch cfd`**.
- **Strategy compatibility for CFD**: momentum / mean_reversion / supertrend (kline-driven) OK; orderbook_scalper not usable (no order-book channel; validator rejects it on cfd).
- **Operational notes**: ① CFD account balance was withdrawn to 0.00 on 2026-08-15 (user intent); two leftover trigger orders from the 08-14 manual verification (buy@4295 id 17511143, sell@4428 id 17471679) could NOT be cancelled while the market was closed (`NOT_IN_TRADE`) — retry `tools/test_gate_rest --tradfi-cleanup` after the market reopens (~08-17). ② 16→17: MCP tool count, JsonRpcServer registry, AGENTS.md/README/OPERATIONAL_GUIDE all updated. ③ nohup logging: stdout only (run.sh console sink); `logs/app.log` may not receive the new run's lines.

Then: rebuild + restart engine → live verification (`get_status` active_market=futures, `get_market XAUUSD` works, `open_order` cfd rejected with 3008, `switch cfd` pauses futures + cancels orders) → manual 0.01-lot verification → small-capital live trading → production hardening

### M16 Maker-First Orders (2026-08-16, done — commits b6f785f / deb5a1e)
- **Config**: per-instance `order_type = "market"|"post_only"|"maker_first"` + `maker_timeout_ms` (ms; > 0 required for maker_first). Validator rejects non-market order_type on `cfd` — TradFi API only has `price_type: market|trigger`, no post-only.
- **Signal flow**: post_only/maker_first signals place `OrderType::PostOnly` at the exact best bid (buy) / best ask (sell) from `OrderBookManager` (futures + spot WS books; futures ticker bid/ask are hardcoded 0). No book data → maker_first falls back to market; post_only drops the signal (never crosses).
- **Sweep**: main loop calls `order_flow.sweepMakerAttempts()` every 200 ms. Expired attempts: cancel (under rest_mutex) → release reservation but KEEP the entry with `reservation_id = 0` (late Cancelled reports still open partial fills with correct futures metadata; `consumeReservation(0)` is a no-op) → fresh 1-arg `placeOrder` for the **remaining** qty (new token + reservation; direction gate / drawdown re-checked).
- **No-chase**: exchange-rejected post-only never falls back to taker; failed cancel (already filled) never falls back. Partial fill + fallback converge to the intended total.
- `OrderType::MakerFirst` is config-only — requests in flight are always Market/PostOnly/Limit. ctor gained two nullable `OrderBookManager*` (spot/futures).

### Engine Ops (2026-08-16, same session — commits d0b305c / 0472f82 / cc381c9)
- **display_timezone**: `[control] display_timezone` ("local"/"utc"/±HH:MM) → `*_str` time fields in JSON-RPC output + REPL time column (src/core/TimeUtil.hpp).
- **Single instance**: flock on `data/engine.lock`; second engine refuses to start (`PULSE_ALLOW_MULTI_INSTANCES=1` bypasses). systemd user service `pulsetrader.service` in deploy/ (loginctl enable-linger + Restart=on-failure + journald).
- **Startup reconciliation**: `GateRestClient::getFuturesPositions` (filters size=0) + `PositionManager::syncPositionFromExchange` (no limit checks, idempotent, `_sync` position id, keeps exchange open_time/liq/leverage); main.cpp syncs at startup (warn-only on failure). Fixed leverage-string crash: `leverage` is a string ("0") in positions JSON.
- **Limits** (user decision, SKHY manual 5051 notional): maxPositionNotional 500→6000, maxSymbolNotional 300→5500, maxOpenPositions 3→4. Engine now systemd-managed (single instance owns 8081/MCP). New-position budget ≈928 USDT.
- **Origin**: dual-engine incident (two engines each placed 3 BTC shorts, phone showed 6) — root-caused, killed the extra engine, fixed above. 1 futures contract = 0.0001 BTC (quanto_multiplier live-verified); 0.0006 = 6 contracts, phone was right.

## Control Plane (L9, `headless` branch)

- **Single binary** `apps/pulsetrader/pulsetrader`, subcommands: `trade` (default; engine + control socket + embedded REPL when stdin is a TTY), `cli` (remote-attach REPL over control socket), `mcp` (stdio MCP server bridging to control socket; auto-loads trading.toml)
- **Control socket**: TCP 127.0.0.1:8081, newline-delimited JSON-RPC 2.0; `[control]` toml (enabled/bindAddress/port), env `PULSE_CONTROL_PORT`
- **Security**: binds localhost-only, no auth — never expose. MCP mode forces file-only logging (stdout = protocol). REST calls serialized via shared mutex in EngineServices
- **16 methods** (method name = MCP tool name): get_status · get_account · get_positions · get_orders · list_strategies · get_strategy_params · set_strategy_param · open_order · close_position · cancel_order · halt_trading · resume_trading · get_risk · get_market · pause_strategy · resume_strategy
- **REPL commands**: status · account|balance · positions · orders · strategies · params <id> · set <id> <param> <value> · open <sym> <buy|sell> <qty> [--type market|limit|post_only] [--price P] [--market spot|futures] [--leverage N] [--reduce-only] [--client-id S] · close <position_id> [qty] [price] · cancel <order_id> · halt · resume · pause <id> · resume-strategy <id> · risk · market <sym> [--levels N] [--klines N] · help · quit
- **New capabilities**: per-strategy runtime pause (`StrategyManager::setPaused`), manual trading halt (`halt_trading`/`resume_trading`), live atomic param get/set; order flow unified in `OrderFlowExecutor` (shared by signal aggregator + manual orders)
- **src/control/**: JsonRpcServer · CommandParser · McpServer · ControlClient · EngineServices · OrderFlowExecutor

## Config Structure

Key files: `src/core/config.hpp` (all structs), `config_loader.cpp` (TOML→struct), `config_validator.cpp` (semantic rules)

```
PulseConfig
├── ExchangeConfig   (apiKey, apiSecret, restBaseUrl, wsUrl, futuresWsUrl, proxyUrl, testnet)
├── LogConfig        (level, logDir, toConsole, toFile)
├── StrategyConfig   (aggregator_threshold, cooldown_sec, instances[])
│   └── StrategyInstanceConfig (name, symbol, market_type, leverage, margin_mode, ...)
├── RiskConfig       (maxPositionNotional, maxOpenPositions, maxDailyDrawdown, max_leverage, ...)
│   ├── StopLossConfig  (mode, fixed_pct, trailing_pct, max_hold_seconds)
│   └── TakeProfitConfig (targets_pct[], fractions[])
├── AiConfig         (backend, model, apiKey, heartbeatIntervalSec)
├── ControlConfig    (enabled, bindAddress, port) — `[control]` TOML, PULSE_CONTROL_PORT env
├── SqliteConfig     (enabled, dbPath)
└── symbols[]
```

## Error Code Ranges

| Range | Subsystem |
|-------|-----------|
| 1xxx | Network (timeout, disconnect, WS, auth) |
| 2xxx | Exchange (rate limit, balance, invalid order) |
| 3xxx | Risk (rejected, drawdown, position limit, stops) |
| 4xxx | AI |
| 5xxx | Config |
| 6xxx | Trade Recorder |
| 7xxx | Futures (leverage, margin, liquidation, funding, contract) |
| 9xxx | Internal / Control plane (91xx) |

## Operational Setup

- **Branch**: `main` + `headless` (WebUI removed; control plane added)
- **run.sh**: `./run.sh {trade|cli|mcp|rest|ws|market|strategy|ai|test}`
- **run.sh trade**: auto-loads `trading.toml` if no `--config` specified
- **.env**: `PULSE_NETWORK` (mainnet/testnet), `GATE_MAINNET_*`, `GATE_TESTNET_*`, `HTTPS_PROXY`
- **Git proxy**: `http.proxy` / `https.proxy` = `http://127.0.0.1:7897`
- ⚠️ **Mainnet** — real money at risk when `PULSE_NETWORK=mainnet`
- ✅ **Testnet** — virtual funds when `PULSE_NETWORK=testnet` (futures only)

## Code Conventions

- `.clang-format`: Allman braces, 120 col, 4-space indent
- Naming: PascalCase classes (no underscores), camelCase functions/methods (no underscores), m_camelCase member variables, kPascalCase constants. Pure-data struct fields keep snake_case.
- File naming: filenames match primary class name (e.g., `OrderExecutor.hpp` for `class OrderExecutor`). Multi-type modules keep descriptive names (`config.hpp`, `types.hpp`, `risk_types.hpp`).
- Yoda conditions, mandatory braces, `Result<T>` = `std::variant<T, PulseError>`
- `ExchangeConfig.restBaseUrl` = host only (`https://api.gateio.ws`), path includes `/api/v4`

## Notes

- QuantX (`~/1_Code/QuantX`) has reusable Gate.io code (signing, REST, futures adapter)
- Sub-account recommended for risk isolation (max 10 for VIP0-4, inherit main VIP)
- Futures: USDT-settled only, leverage up to 125x, simultaneous spot+futures via config
