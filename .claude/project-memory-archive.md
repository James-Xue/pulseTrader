# pulseTrader — Historical Details (Secondary Memory)

> This file stores historical details migrated from project-memory.md.
> Read on demand when reviewing implementation details of completed modules.

## Trading Engine (M5, 2026-06-19)

- **`apps/pulsetrader/main.cpp`**: ~630 lines, wiring all 9 layers
  - Construction order: L2 Logger → L1 Exchange → L3 Market → L7 Risk → L8 Execution → L6 Strategy → L4 AI → L5 Heartbeat → L9 WebUI
  - Signal flow: StrategyManager → SignalAggregator → [app callback: risk check → OrderExecutor → OrderTracker]
  - OrderTracker completion callback → PositionManager open/close + DrawdownGuard PnL
  - Graceful shutdown: SIGINT/SIGTERM → atomic stop flag → reverse-order stop (WebUI → Heartbeat → Strategy → Market → WS → Logger)
  - Strategy factory: `create_strategy()` maps config name → concrete class (MomentumScalper, OrderBookScalper, MeanReversionScalper)
  - Default config: 2 strategies on BTC_USDT, AI disabled, WebUI on :8080, credentials from `.env`
- **`docs/OPERATIONAL_GUIDE.md`**: 630-line operational guide

## TOML Config Loader (M6, 2026-06-19)

- **`src/core/config_loader.hpp/cpp`**: toml11 v4, four-stage pipeline: file check → TOML parse → `from_env:VAR` resolution → section parsers
  - `from_env:` reads sensitive values from env vars; unset/empty → empty string
  - `find_double()` helper handles toml11 v4 integer/float type distinction
  - All fields optional, unknown keys silently ignored
- **`src/core/config_validator.hpp/cpp`**: 20+ semantic rules (required fields, risk ranges, stop-loss/take-profit consistency, strategy symbols validation)
- **Error codes**: 5xxx range (ConfigFileNotFound, ConfigParseError, ConfigMissingField, ConfigInvalidValue, ConfigEnvVarMissing, ConfigValidationError)
- CMake: toml11 → mandatory core; `pulse_core` INTERFACE→STATIC
- CLI: `--config <path>` flag; `./run.sh trade --config trading.toml`

## WS JSON Parsing Fix (2026-06-19)

- **Orderbook**: Gate.io v4 WS sends numerics as JSON strings. Fix: `is_string()` branch with `std::stod()`/`std::stoull()`.
- **Kline timestamp**: `result["t"]` is string. Fix: `is_string()` with `std::stoll()`.
- **Sequence gap**: `lastUpdateId` is global counter, not per-symbol. Fix: accept any `delta_seq > last_seq`, reject stale.

## SQLite Trade Recorder (M7, 2026-06-19)

- **`src/trade_recorder/`**: `TradeRecord` (17 fields), `TradeSummary` (aggregate), RAII `TradeRecorder`
  - Factory `open(db_path)` with WAL + `synchronous=NORMAL`
  - `record_trade(ExecutionReport, pnl, strategy_name)` — mutex-guarded, UNIQUE order_id
  - 4 queries: `get_trades`, `get_trades_by_strategy`, `get_summary`, `get_daily_pnl`
  - Namespace: `SQLite::` (not `SQLiteCpp::`)
- **Table**: 17 columns + 3 indexes
- **Error codes**: 6xxx range
- **OrderTracker**: `client_order_id` added, `main.cpp` passes `sig.strategy_id`
- 27 tests (`:memory:` SQLite)

## Futures Config M8 (2026-06-19)

- **types.hpp**: `MarketType` (Spot/Futures), `MarginMode` (Cross/Isolated), `to_string()` helpers
- **config.hpp**: `ExchangeConfig.futuresWsUrl`, `StrategyInstanceConfig.market_type/leverage/margin_mode`, `RiskConfig.max_leverage/max_margin_used`, `PulseConfig.default_market_type`
- **error.hpp**: 7xxx range (FuturesLeverageExceeded 7001–FuturesContractNotFound 7005)
- Config loader: `parse_market_type()`/`parse_margin_mode()` helpers
- Validator: leverage range, max_leverage 1.0–125.0, max_margin_used 0.0–1.0
- All defaults backward-compatible (Spot, leverage=1.0, Cross)
- 18 tests

## Bug Fixes (2026-06-18)

- REST URL double path: `restBaseUrl` → host only (`https://api.gateio.ws`)
- WS subscribe race: queue `PendingAction`, send immediately if connected
- WS pong missing: `on_message` replies `spot.pong` immediately
- Orderbook symbol field: `result.value("s", "")`
- Orderbook event type: `"update"` → `"all"` for snapshot detection

## Completed Layers

| Layer | Date | Tests | Notes |
|-------|------|-------|-------|
| L2 Logging | 06-15 | 8 | spdlog async, per-module isolation, `PULSE_LOG_*` macros |
| L1 Exchange REST | 06-16/17 | 11 | libcurl + HMAC signing + retry + proxy |
| L1 Exchange WS | 06-16/17 | 24 | websocketpp + asio, auto-reconnect, proxy tunnel, HMAC auth |
| L3 Market Data | 06-16 | 33 | ticker_cache, symbol_registry, kline_buffer (seqlock), orderbook_manager |
| L8 Execution | 06-16 | 22 | order_executor (REST), order_tracker (WS + REST fallback) |
| L7 Risk | 06-16 | 92 | position_manager, drawdown_guard, order_rate_limiter, risk_manager, stops |
| L6 Strategy | 06-16 | 52 | 3 strategies, signal_aggregator, strategy_manager (jthread per strategy) |
| L4 AI | 06-17 | 43 | ai_pipeline, twitter/news_feed, prompt_builder, ai_client, param_advisor |
| L5 Heartbeat | 06-17 | 7 | task_queue, heartbeat_scheduler (asio steady_timer) |
| L9 WebUI | 06-17 | 57 | dashboard_state, web_server (uWebSockets), ws_server, dark-theme SPA |

## Key Design Decisions

- Two parallel data pipelines: market hot path (latency-critical) vs AI background, bridged via `std::atomic`
- WebUI: layered polling (200ms~5min), lock-free reads (atomic/seqlock/atomic shared_ptr)
- WebUI: uWebSockets (crow/beast conflict with standalone asio), localhost + bearer token + Host header
- WebUI gated by CMake: `-DPULSE_ENABLE_WEBUI=ON`
- HTTP proxy: REST via `CURLOPT_PROXY`; WS via `ProxyTunnel` class (TCP forwarder + HTTP CONNECT)
- SQLiteCpp: build from source with `-DCMAKE_CXX_FLAGS="-include cstdint"` for GCC 15

## Strategic Decisions (2026-04-05)

- Open-source for tech reputation over personal trading profit
- Infrastructure open-source, strategy layer private
- Goal: complete market-data → order-execution pipeline before promoting

## Full Roadmap History

1. ✅ L2 Logging (M1 prerequisite)
2. ✅ L1 Exchange REST
3. ✅ L1 Exchange WebSocket
4. ✅ L3 Market Data → **M1**
5. ✅ L8 Execution → **M1**
6. ✅ L7 Risk Management → **M2**
7. ✅ L6 Strategy Engine → **M2**
8. ✅ L5 + L4 AI → **M3**
9. ✅ L9 WebUI → **M4**
10. ✅ Trading Engine → **M5**
11. ✅ TOML Config → **M6** (46 tests, 404 total)
12. ✅ SQLite Trade Recorder → **M7** (27 tests, 431 total)
13. ✅ Futures Config → **M8** (18 tests, 449 total)
14. ✅ EndpointRouter + WS Ping → **M9** (18 tests, 467 total)

## Moved from project-memory.md (compression, 2026-08-17)

### Post-M13 Bugfixes (2026-06-20)
- **Ctrl+C graceful shutdown** — 3-layer fix in `gate_ws_client.cpp`:
  1. `GateWsClient::stop()`: `io_ctx_ptr->stop()` to force-stop asio event loop (unblocks `client.run()`)
  2. `ProxyTunnel` accept thread: `poll()` + 200ms timeout instead of blocking `accept()` (Linux `close()` can't interrupt blocking `accept()`)
  3. `ProxyTunnel` relay threads: no longer `detach()`; `stop()` closes sockets then `join()`s all relay threads
  4. `run_io_loop()`: explicit `tunnel->stop()` + `tunnel.reset()` before function return (correct cleanup order)
  5. `WsInternal`: added `io_ctx_ptr` field, set after `init_asio()`, cleared after `client.run()` returns
- **Strategy warmup diagnostics** — kline strategies log "Waiting for kline data" / "Warming up: X/N candles" every 30s during cold start (new `last_warmup_log_ms_`/`last_no_data_log_ms_` members)
- **Aggregator threshold lowered** — `signal_aggregator_threshold` 0.7 → 0.6 to match single-strategy min_confidence
- **OPERATIONAL_GUIDE.md updated** — §4.4 warmup explanation, §5.1 threshold tuning, Q7 "No orders placed after startup?" checklist

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
