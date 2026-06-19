# pulseTrader — Project Memory

> Last updated: 2026-06-19
> 文件大小：10563 字符 / 16000 字符。更新本文件后必须重新计算并同步这一行。

## Overview

- **Project**: pulseTrader — C++20 high-frequency scalping framework
- **Version**: 0.1.0-dev
- **Repository**: https://github.com/James-Xue/pulseTrader (public, GPL 3.0)
- **Exchange**: Gate.io (REST + WebSocket), single-exchange focus
- **Namespace**: `pulse::`
- **Build**: CMake + vcpkg

## Architecture (9 Layers)

| Layer | Module | Responsibility | Status |
|-------|--------|----------------|--------|
| 1 | Exchange | Gate.io REST + WebSocket API | ✅ Done |
| 2 | Logging & Monitoring | spdlog + fmt | ✅ Done |
| 3 | Market Data | Hot path, latency-critical | ✅ Done |
| 4 | AI Analysis | Social/news ingestion → LLM → fixed JSON schema → param deltas | ✅ Done |
| 5 | Heartbeat Scheduler | 5-min AI clock, TaskQueue worker thread | ✅ Done |
| 6 | Strategy Engine | EMA crossover, order book imbalance, Bollinger Band mean-reversion, weighted signal aggregation | ✅ Done |
| 7 | Risk Management | PositionManager, RiskManager, DrawdownGuard, OrderRateLimiter, StopLossEngine, TakeProfitEngine | ✅ Done |
| 8 | Order Execution | Order lifecycle management | ✅ Done |
| 9 | WebUI | Cross-cutting read-only observability | ✅ Done |

### Key Design Decisions
- Two parallel data pipelines: market hot path (latency-critical) vs AI background pipeline, bridged via `std::atomic`
- WebUI: layered polling (200ms ~ 5min), lock-free reads (atomic load / seqlock / atomic shared_ptr)
- WebUI recommended stack: uWebSockets (crow/beast conflict with standalone asio)
- WebUI security: localhost bind + bearer token + Host header validation
- WebUI gated by CMake flag: `-DPULSE_ENABLE_WEBUI=ON`
- HTTP proxy support: REST via libcurl `CURLOPT_PROXY` + `CURLOPT_HTTPPROXYTUNNEL`; WebSocket via `ProxyTunnel` class (local TCP forwarder + HTTP CONNECT tunnel)
- API credentials: `.env` file with `GATE_API_KEY` / `GATE_API_SECRET` env vars (gitignored)

## Dependencies (vcpkg.json)

- Core: nlohmann-json, spdlog, fmt, curl, openssl, asio, websocketpp, gtest
- Optional: sqlitecpp (`-DPULSE_ENABLE_SQLITE=ON`), toml11 (`-DPULSE_ENABLE_TOML=ON`), uwebsockets (`-DPULSE_ENABLE_WEBUI=ON`)
- Vendored: uWebSockets + uSockets in `third_party/` (built from source with epoll backend, no libuv needed)

## Current State (2026-06-19)

### Trading Engine (2026-06-19)
- **`apps/pulsetrader/main.cpp`**: Main trading program wiring all 9 layers (~630 lines)
  - Construction order: L2 Logger → L1 Exchange → L3 Market → L7 Risk → L8 Execution → L6 Strategy → L4 AI → L5 Heartbeat → L9 WebUI
  - Signal flow: StrategyManager → SignalAggregator → [app callback: risk check → OrderExecutor → OrderTracker]
  - OrderTracker completion callback → PositionManager open/close + DrawdownGuard PnL
  - Graceful shutdown: SIGINT/SIGTERM → atomic stop flag → reverse-order stop (WebUI → Heartbeat → Strategy → Market → WS → Logger)
  - Strategy factory: `create_strategy()` maps config name → concrete class (MomentumScalper, OrderBookScalper, MeanReversionScalper)
  - Default config: 2 strategies on BTC_USDT, AI disabled, WebUI on :8080, credentials from `.env`
- **`apps/pulsetrader/CMakeLists.txt`**: Build rules linking all layer libraries, conditional WEBUI support
- **`run.sh`**: Added `./run.sh trade` command to launch the trading engine
- **`docs/OPERATIONAL_GUIDE.md`**: 598-line operational guide covering setup, config, parameter tuning, risk control, profitability analysis, FAQ

### WS JSON Parsing Fix (2026-06-19)
- **Orderbook price/quantity type mismatch** (`orderbook_manager.cpp`): Gate.io v4 WS sends all numeric values as JSON strings (e.g. `"50000.0"`). `parse_levels()` and `apply_delta_levels()` used `.get<Price>()` / `.get<Quantity>()` which threw `json.exception.type_error.302`. Fix: `is_string()` branch — `std::stod(level[0].get<std::string>())` for strings, direct `.get<>()` for numbers.
- **Kline timestamp type mismatch** (`market_feed.cpp:207`): `result["t"]` is a string, `.get<std::int64_t>()` crashed. Fix: `is_string()` branch with `std::stoll()`.
- **`lastUpdateId` type hardening** (`orderbook_manager.cpp:28,57`): Added `is_string()` fallback with `std::stoull()` for both snapshot and delta sequence IDs.
- **Sequence gap logic** (`orderbook_manager.cpp`): Gate.io's `lastUpdateId` is a **global counter** shared across all symbols' order book updates, not a per-symbol sequential ID. Old code expected `last_seq + 1` and re-subscribed on every gap — caused infinite re-subscribe loop. Fix: accept any `delta_seq > last_seq`, only reject stale deltas (`delta_seq <= last_seq`).
- **Test update**: Replaced `SequenceGapTriggersResubscribe` with `SequenceGapIsAccepted` + `StaleDeltaIsRejected` (2 new tests).

### Bug Fixes (2026-06-18)
- **REST URL double path** (`config.hpp`): `restBaseUrl` changed to host only (`https://api.gateio.ws`), path includes `/api/v4`.
- **WS subscribe race condition** (`gate_ws_client.cpp`): `subscribe()` now queues `PendingAction` and sends immediately if connected. Refactored `WsInternal` to member `shared_ptr`.
- **WS pong missing**: `on_message` now replies `spot.pong` immediately.
- **Orderbook symbol field** (`market_feed.cpp`): Changed to `result.value("s", "")` (Gate.io puts symbol in `result["s"]`).
- **Orderbook event type** (`market_feed.cpp`): Changed `"update"` to `"all"` for snapshot detection.

### Completed
- **L2 Logging** (2026-06-15): spdlog async, per-module isolation, `PULSE_LOG_*` macros, 8 tests
- **L1 Exchange REST** (2026-06-16/17): Gate.io v4 REST, libcurl + HMAC signing + retry + proxy, 11 tests
- **L1 Exchange WebSocket** (2026-06-16/17): websocketpp + asio, auto-reconnect, proxy tunnel, private HMAC auth, 24 tests
- **L3 Market Data** (2026-06-16): ticker_cache, symbol_registry, kline_buffer (seqlock), orderbook_manager, market_feed, 33 tests
- **L8 Order Execution** (2026-06-16): order_executor (REST), order_tracker (WS + REST fallback), execution_report, 22 tests
- **L7 Risk Management** (2026-06-16): position_manager, drawdown_guard, order_rate_limiter, risk_manager, stop_loss/take_profit engines, 92 tests
- **L6 Strategy Engine** (2026-06-16): 3 strategies (momentum/orderbook/mean_reversion), signal_aggregator, strategy_manager (jthread per strategy), 52 tests
- **L4 AI Analysis** (2026-06-17): ai_pipeline, twitter_feed, news_feed, prompt_builder, ai_client (OpenAI/Claude), param_advisor, 43 tests
- **L5 Heartbeat** (2026-06-17): task_queue, heartbeat_scheduler (asio steady_timer), 7 tests
- **L9 WebUI** (2026-06-17): dashboard_state (tiered polling), web_server (uWebSockets), ws_server (push broadcast), dark-theme SPA, 57 tests
- **Coding standards** (2026-06-15/16): AGENTS.md, Allman braces, Yoda conditions, mandatory braces

### Test Summary
- 358 tests total (with WEBUI): core 9 + logger 8 + exchange 35 + market 33 + execution 22 + risk 92 + strategy 52 + AI 43 + heartbeat 7 + webui 57 — all passing
- 320 tests (without WEBUI): same minus webui — all passing

### Milestones Achieved
- **M1** ✅: End-to-end Exchange → Market Data → Execution pipeline
- **M2** ✅: Automatic trading: Market Data → Strategy → Risk → Execution
- **M3** ✅: AI adaptive — strategy parameters auto-tune every 5 min via LLM analysis
- **M4** ✅: Complete product — all 9 layers operational, WebUI dashboard with real-time monitoring
- **M5** ✅: Trading engine — all 9 layers wired into runnable process, `./run.sh trade` launches full system

### Next Steps (per roadmap)
- M1–M5 achieved. Future enhancements: TOML config file loading, SQLite trade recorder, MetricsCollector (L2), TLS support, strategy parameter tuning via WebUI, backtesting system.

### Operational Setup (2026-06-17)
- **Branch status**: All feature branches merged into `main` and deleted (local + remote). Only `main` branch exists.
- **run.sh**: Convenience script in project root — `./run.sh {trade|rest|ws|market|strategy|ai|webui|test}`
  - Auto-sources `.env` for API credentials and proxy settings
  - Commands: **trade** (trading engine — 9 layers), rest, ws, market, strategy, ai (mock), webui, test (358 unit tests)
- **.env**: Gitignored file for runtime configuration
  - `GATE_API_KEY` / `GATE_API_SECRET` — Gate.io HMAC credentials
  - `HTTP_PROXY` / `HTTPS_PROXY` — Clash Verge proxy (`http://127.0.0.1:7897`)
- **.gitignore**: Ignores `logs/`, `Testing/`, `.env`, `build/`, `.claude/`
- **Git global proxy**: `http.proxy` and `https.proxy` set to `http://127.0.0.1:7897`
- **Warning**: Current config uses **mainnet** (not testnet). Real money at risk when placing orders.

## Code Conventions

- `.clang-format`: Allman braces, Cpp11BracedListStyle=false, 120 col limit, 4-space indent
- Naming: PascalCase classes, snake_case functions/vars, kPascalCase constants, trailing_underscore_ privates
- Yoda conditions: `if (0 == status)` not `if (status == 0)`
- All braces mandatory (even single-line if/for/while)
- Error handling: `Result<T>` = `std::variant<T, PulseError>`, ErrorCode enum by subsystem range
- Config: `ExchangeConfig` stores `restBaseUrl` as host only (e.g. `https://api.gateio.ws`), path includes `/api/v4`

## Strategic Decisions (from 2026-04-05)

- **Open-source for tech reputation** over personal trading profit (alpha decay, small capital, institution competition)
- Infrastructure layer open-source, strategy layer can stay private
- Goal: complete market-data → order-execution pipeline before promoting

## Roadmap (from implementation-roadmap.md)

1. ✅ Phase 1 Step 1.1: L2 Logging
2. ✅ Phase 1 Step 1.2: L1 Exchange REST
3. ✅ Phase 1 Step 1.3: L1 Exchange WebSocket
4. ✅ Phase 2: L3 Market Data Pipeline
5. ✅ Phase 3: L8 Order Execution → **Milestone M1 achieved**
6. ✅ Phase 4: L7 Risk Management — 6 modules, 92 tests
7. ✅ Phase 5: L6 Strategy Engine → 3 strategies, signal aggregator, 52 tests
8. ✅ Phase 6: L5 + L4 AI Pipeline → **Milestone M3 achieved** (AI adaptive, 50 new tests)
9. ✅ Phase 7: L9 WebUI → **Milestone M4 achieved** (DashboardState + WebServer + WsServer + Frontend SPA, 57 new tests)
10. ✅ Phase 8: Trading Engine → **Milestone M5 achieved** (apps/pulsetrader/main.cpp, 9-layer wiring, WS JSON fix, operational guide)

## Notes

- Project originally at `~/1_Code/pulseTrader`, moved to `~/1_Code/09_pulseTrader`
- QuantX (`~/1_Code/QuantX`) has reusable Gate.io code (signing, REST client, futures adapter)
- Sub-account API trading recommended for risk isolation (one sub-account per strategy)
- Gate.io sub-accounts: inherit main VIP, max 10 (VIP0-4) or 30 (VIP5-9), cannot delete once created
