# pulseTrader — Project Memory

> Last updated: 2026-06-20
> 文件大小：9911 字符 / 20000 字符。更新本文件后必须重新计算并同步这一行。
> 历史细节已迁移至 `project-memory-archive.md`

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

## Current State (M13 Done, 2026-06-20)

### Test Summary
- **528 tests** (WEBUI + SQLITE): core 25 (+10 safe_parse_double) + config_loader 30 + config_validator 34 + logger 8 + exchange 66 (+7 proxy_tunnel) + market 38 + execution 29 (+3 callback_safety) + risk 112 (+5 atomic_reserve) + strategy 59 + AI 43 + heartbeat 7 + webui 57 + trade_recorder 27
- 501 without SQLITE · 463 without WEBUI or SQLITE

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
- **OPERATIONAL_GUIDE.md 更新** — §4.4 添加策略预热期说明（日志示例 + "至少等 25 分钟"）、§5.1 更新 threshold 调优说明、新增 Q7"启动后一直没有下单？"排查清单

### Architecture Review Fixes (2026-06-20)
- **#1 PnL 接通 DrawdownGuard** (`c857e21`): `close_position()` 返回 `optional<double>` 已实现 PnL，main.cpp 累加后传给 `drawdown_guard.record_pnl()`。回撤保护现已生效。
- **#2 AI 反馈回路接通** (`786e9f8`): `StrategyManager.all_params()` 收集每个策略的真实 params 指针，`AiPipeline::run()` 改为 `vector<StrategyParams*>&`，ParamAdvisor 循环写入所有策略的 atomic params。
- **#3 stod 防崩溃** (`7c052cf`): 新增 `safe_parse_double()` (基于 `std::from_chars`，无异常，locale-independent)，替换 34 处 `std::stod` 调用 + 10 个新测试。测试总数 513。

### Next: Architecture Review Remaining + Testnet
- ✅ #4 RiskManager TOCTOU — `PositionManager::reserve_notional()` 原子预留模式，单次 unique_lock 替代 3 次独立 shared_lock。`RiskEvalResult` 新增 `reservation_id`，`main.cpp` 失败路径 `cancel_reservation()`，成功路径自动消耗。5 新测试。
- ✅ #5 OrderTracker 写锁下回调 — "锁内收集，锁外执行"模式：`process_order_update()` 和 `poll_order_status()` 的 `completion_callback_` 在 unique_lock 释放后调用。`set_completion_callback()` 加锁保护。新增 `test_simulate_ws_update()` / `test_try_shared_lock()` 测试接口。3 新测试。
- ✅ #6 ProxyTunnel 提取 — 373 行网络代码从 `gate_ws_client.cpp` 提取为 `proxy_tunnel.hpp/.cpp`。修复 2 个隐藏 bug：(1) `handle_connection` 线程从 `.detach()` 改为可 join；(2) relay socket/thread 注册合并为单个 lock_guard 作用域。删除 58 行死代码（SSL relay 重载）。7 新测试。
- Run testnet for 1 week, collect strategy performance data
- Verify signal quality and PnL in virtual fund environment
- WebUI: http://127.0.0.1:8080 for real-time monitoring

Then: P&L analysis → 小资金实盘 → production hardening

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
