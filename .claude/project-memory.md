# pulseTrader — Project Memory

> Last updated: 2026-06-21
> 文件大小：11727 字符 / 20000 字符。更新本文件后必须重新计算并同步这一行。
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
- **OPERATIONAL_GUIDE.md 更新** — §4.4 添加策略预热期说明（日志示例 + "至少等 25 分钟"）、§5.1 更新 threshold 调优说明、新增 Q7"启动后一直没有下单？"排查清单

### Architecture Review Fixes (2026-06-20)
- **#1 PnL 接通 DrawdownGuard** (`c857e21`): `close_position()` 返回 `optional<double>` 已实现 PnL，main.cpp 累加后传给 `drawdown_guard.record_pnl()`。回撤保护现已生效。
- **#2 AI 反馈回路接通** (`786e9f8`): `StrategyManager.all_params()` 收集每个策略的真实 params 指针，`AiPipeline::run()` 改为 `vector<StrategyParams*>&`，ParamAdvisor 循环写入所有策略的 atomic params。
- **#3 stod 防崩溃** (`7c052cf`): 新增 `safe_parse_double()` (基于 `std::from_chars`，无异常，locale-independent)，替换 34 处 `std::stod` 调用 + 10 个新测试。测试总数 513。

### Testnet URL Auto-Switch Fix (2026-06-21)
- **问题**: `testnet=true` 时只有 REST URL 切换到测试网，WS URL 保持主网（私有频道认证失败）
- **方案**: `config_loader.cpp` 调整加载顺序 — 先读 `testnet` flag → 按网络模式设 URL 默认值 → 再用 `find_or` 加载 URL 字段
- **效果**: `testnet=true` 时 REST/Spot WS/Futures WS 全部自动切换为测试网地址；显式 TOML URL 可覆盖（中国用户回退主网 WS）
- **改动**: `config.hpp` 新增 `pulse::url` 命名空间（6 个 URL 常量）; `config_loader.cpp` 调整 `parse_exchange()` 加载顺序; `main.cpp` 删除旧 override 块 + banner 显示全部 3 URL; `trading.toml` 简化（删除显式 URL）
- **新增 4 测试**: `TestnetAutoSwitch`, `TestnetExplicitOverride`, `MainnetDefault`, `MainnetExplicit`
- **笔记本环境搭建**: apt 安装 libasio-dev 1.30.2 + libwebsocketpp-dev 0.8.2+git20250909 + libsqlitecpp-dev 3.3.3 + toml11 4.4.0（~/.local）
- 532 测试全绿（原 528 + 4 新测试）

### Testnet WS CloudFront TLS 不兼容 (2026-06-21)
- **问题**: 测试网行情 WS `wss://ws-testnet.gate.com/v4/ws/futures/usdt` 在 CloudFront 后面，TLS 握手时 HTTP/2 协商导致 websocketpp 0.8.3-dev 报 `Invalid HTTP status`
- **诊断**: Python raw socket 直连返回 `HTTP/1.1 101` ✅，但 websocketpp 通过 ProxyTunnel 无法解析响应（服务器返回 3535 字节 Amazon 证书 + DOWNGRD 标记）
- **方案**: `kTestnetFuturesWs` 改回主网 URL `wss://fx-ws.gateio.ws/v4/ws/usdt`（行情数据主网/测试网完全一致），REST 仍走测试网
- **TOML**: `trading.toml` 显式列出 3 个 URL，便于用户按需覆盖
- **commit**: `0e61877`

### System Heartbeat Logging (2026-06-21)
- **问题**: 预热结束后系统完全静默，无法确认是否存活
- **方案**: MarketFeed 添加 `FeedStats` 原子计数器（ticker/orderbook/kline），main loop 每 60 秒打印系统状态摘要
- **日志格式**: `[heartbeat] uptime 1h23m | futures 100 tick/s  10 kline/s  80 ob/s | ws spot=n/a futures=connected | strategies 3/3 running | positions 0 (notional 0.00 USDT)`
- **性能**: 热路径仅 `fetch_add(1, relaxed)` ~1 cycle；60 秒一次日志在主线程（原本空闲）
- **改动**: `market_feed.hpp` (+FeedStats +3 atomic) · `market_feed.cpp` (+计数器 +stats()) · `main.cpp` (+log_system_heartbeat +修改主循环)
- 5 新测试 (FeedStats 初始化/并发/delta 计算)
- 537 测试全绿

### WebUI K 线图修复 (2026-06-21)
- **Bug #1 (致命)**: `on_kline_update()` 从 `full_frame["contract"]` 提取合约名，但 futures candlesticks 外层帧没有 `contract` 字段，合约名在 `result["n"]` 中 → 100% futures K 线数据被静默丢弃
- **Bug #2 (高)**: K 线订阅 payload 顺序错误 `["BTC_USDT", "1m"]`，Gate.io 要求 `["1m", "BTC_USDT"]`（interval 在前）
- **Bug #3 (低)**: `poll_klines()` 仅在 `open_time` 变化时推送快照（~60 秒一次），当前 K 线的 OHLCV 变化不反映到前端。新增 `last_kline_close_` map 检测价格变动
- **分析文档**: `docs/kline-bug-analysis-2026-06-21.md`
- 3 新测试 (spot/futures kline JSON 格式 + payload 顺序)
- 540 测试全绿

### Next Steps
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
