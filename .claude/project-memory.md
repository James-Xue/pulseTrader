# pulseTrader — Project Memory

> Last updated: 2026-06-21
> File size: 14969 chars / 20000 chars. Must recalculate and sync this line after updating this file.
> Historical details migrated to `project-memory-archive.md`

## Overview

- **Project**: pulseTrader — C++20 high-frequency scalping framework
- **Repository**: https://github.com/James-Xue/pulseTrader (public, GPL 3.0)
- **Exchange**: Gate.io (REST + WebSocket), single-exchange focus
- **Namespace**: `pulse::` · **Build**: CMake + vcpkg

## Architecture (9 Layers, All ✅)

| L1 Exchange | L2 Logging | L3 Market Data | L4 AI Analysis | L5 Heartbeat |
|---|---|---|---|---|
| L6 Strategy | L7 Risk Mgmt | L8 Execution | L9 WebUI | — |

- Hot path (L1→L3→L6→L7→L8) vs AI background (L4→L5), bridged via `std::atomic`
- WebUI: uWebSockets, `-DPULSE_ENABLE_WEBUI=ON`, localhost + bearer token
- Proxy: REST via `CURLOPT_PROXY`; WS via `ProxyTunnel` class
- Credentials: `.env` (`GATE_API_KEY`/`GATE_API_SECRET`), gitignored

## Dependencies

- Core: nlohmann-json, spdlog, fmt, curl, openssl, asio, websocketpp, gtest, toml11
- Optional: sqlitecpp (`-DPULSE_ENABLE_SQLITE=ON`), uwebsockets (`-DPULSE_ENABLE_WEBUI=ON`)
- Vendored: uWebSockets + uSockets in `third_party/`
- SQLiteCpp GCC 15 fix: build with `-DCMAKE_CXX_FLAGS="-include cstdint"`

## Current State (M13 Done, 2026-06-21)

### Test Summary
- **540 tests** (WEBUI + SQLITE): core 25 (+10 safe_parse_double) + config_loader 34 (+4 testnet URL) + config_validator 34 + logger 8 + exchange 66 (+7 proxy_tunnel) + market 46 (+8 feed_stats + kline JSON format) + execution 29 (+3 callback_safety) + risk 112 (+5 atomic_reserve) + strategy 59 + AI 43 + heartbeat 7 + webui 57 + trade_recorder 27
- 513 without SQLITE · 475 without WEBUI or SQLITE

### Milestones
- **M1–M5** ✅: Core pipeline → strategy → risk → AI → WebUI → trading engine
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
  - WebUI null guard: uses futures feed/tracker when spot unavailable
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
- **WebUI token caching** — `frontend/app.js`:
  - `getToken()`: URL param → `localStorage('pulseToken')` → `prompt()`, cached on first entry
  - Bootstrap: always `connect()` even with empty token (server skips auth when `authToken` is empty)
  - `trading.toml`: `authToken = ""` for testnet dev mode (no auth prompt at all)
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

### Testnet WS CloudFront TLS Incompatibility (2026-06-21)
- **Problem**: Testnet market data WS `wss://ws-testnet.gate.com/v4/ws/futures/usdt` behind CloudFront; TLS handshake HTTP/2 negotiation causes websocketpp 0.8.3-dev to report `Invalid HTTP status`
- **Diagnosis**: Python raw socket direct connection returns `HTTP/1.1 101` ✅, but websocketpp via ProxyTunnel cannot parse response (server returns 3535-byte Amazon certificate + DOWNGRD flag)
- **Solution**: `kTestnetFuturesWs` changed back to mainnet URL `wss://fx-ws.gateio.ws/v4/ws/usdt` (market data is identical between mainnet/testnet), REST still uses testnet
- **TOML**: `trading.toml` explicitly lists 3 URLs for easy user override
- **commit**: `0e61877`

### System Heartbeat Logging (2026-06-21)
- **Problem**: System completely silent after warmup, no way to confirm it's alive
- **Solution**: MarketFeed added `FeedStats` atomic counters (ticker/orderbook/kline); main loop prints system status summary every 60 seconds
- **Log format**: `[heartbeat] uptime 1h23m | futures 100 tick/s  10 kline/s  80 ob/s | ws spot=n/a futures=connected | strategies 3/3 running | positions 0 (notional 0.00 USDT)`
- **Performance**: Hot path only `fetch_add(1, relaxed)` ~1 cycle; one log per 60s on main thread (otherwise idle)
- **Changes**: `market_feed.hpp` (+FeedStats +3 atomic) · `market_feed.cpp` (+counters +stats()) · `main.cpp` (+log_system_heartbeat +modified main loop)
- 5 new tests (FeedStats initialization/concurrency/delta calculation)
- 537 tests all green

### WebUI K-Line Chart Fix (2026-06-21)
- **Bug #1 (Critical)**: `on_kline_update()` extracts contract name from `full_frame["contract"]`, but futures candlestick outer frame has no `contract` field — contract name is in `result["n"]` → 100% of futures K-line data silently dropped
- **Bug #2 (High)**: K-line subscription payload order wrong `["BTC_USDT", "1m"]`; Gate.io requires `["1m", "BTC_USDT"]` (interval first)
- **Bug #3 (Low)**: `poll_klines()` only pushes snapshot on `open_time` change (~every 60s); OHLCV changes in the current candle are not reflected to the frontend. Added `last_kline_close_` map to detect price changes
- **Analysis doc**: `docs/kline-bug-analysis-2026-06-21.md`
- 3 new tests (spot/futures kline JSON format + payload order)
- 540 tests all green

### Account Balance Display (2026-06-21)
- **AccountBalance struct**: total, available, unrealised_pnl, position_margin, order_margin, currency
- **REST parsing**: `GateRestClient::get_futures_account_balance()` — parses Gate.io futures account JSON (all values as strings → safe_parse_double)
- **DashboardState**: 10-second REST polling for account balance, stored in `AccountSnapshot`
- **WebUI top status bar**: Total / Available / Unrealized PnL / Margin Used (dark theme, 10s refresh)
- **Heartbeat log**: `... | account 1000.00 USDT (avail 950.00, pnl +2.50)`
- `Result<T>` is `std::variant<T, PulseError>` — use `ok()` / `value()` / `error()`, not `has_value()`
- 540 tests all green

### Next Steps
- ✅ #4 RiskManager TOCTOU — `PositionManager::reserve_notional()` atomic reservation mode, single unique_lock replacing 3 independent shared_locks. `RiskEvalResult` added `reservation_id`; `main.cpp` failure path calls `cancel_reservation()`, success path auto-consumes. 5 new tests.
- ✅ #5 OrderTracker Callback Under Write Lock — "collect inside lock, execute outside lock" pattern: `completion_callback_` in `process_order_update()` and `poll_order_status()` called after unique_lock is released. `set_completion_callback()` protected by lock. Added `test_simulate_ws_update()` / `test_try_shared_lock()` test interfaces. 3 new tests.
- ✅ #6 ProxyTunnel Extraction — 373 lines of network code extracted from `gate_ws_client.cpp` into `proxy_tunnel.hpp/.cpp`. Fixed 2 hidden bugs: (1) `handle_connection` thread changed from `.detach()` to joinable; (2) relay socket/thread registration merged into a single lock_guard scope. Removed 58 lines of dead code (SSL relay overloads). 7 new tests.
- Run testnet for 1 week, collect strategy performance data
- Verify signal quality and PnL in virtual fund environment
- WebUI: http://127.0.0.1:8080 for real-time monitoring

Then: P&L analysis → small capital live trading → production hardening

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
├── WebuiConfig      (enabled, bindAddress, port, authToken)
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
| 9xxx | Internal / WebUI |

## Operational Setup

- **Branch**: only `main` exists
- **run.sh**: `./run.sh {trade|rest|ws|market|strategy|ai|webui|test}`
- **run.sh trade**: auto-loads `trading.toml` if no `--config` specified
- **.env**: `PULSE_NETWORK` (mainnet/testnet), `GATE_MAINNET_*`, `GATE_TESTNET_*`, `HTTPS_PROXY`
- **Git proxy**: `http.proxy` / `https.proxy` = `http://127.0.0.1:7897`
- ⚠️ **Mainnet** — real money at risk when `PULSE_NETWORK=mainnet`
- ✅ **Testnet** — virtual funds when `PULSE_NETWORK=testnet` (futures only)

## Code Conventions

- `.clang-format`: Allman braces, 120 col, 4-space indent
- Naming: PascalCase classes, snake_case functions/vars, kPascalCase constants, trailing_underscore_ privates
- Yoda conditions, mandatory braces, `Result<T>` = `std::variant<T, PulseError>`
- `ExchangeConfig.restBaseUrl` = host only (`https://api.gateio.ws`), path includes `/api/v4`

## Notes

- QuantX (`~/1_Code/QuantX`) has reusable Gate.io code (signing, REST, futures adapter)
- Sub-account recommended for risk isolation (max 10 for VIP0-4, inherit main VIP)
- Futures: USDT-settled only, leverage up to 125x, simultaneous spot+futures via config
