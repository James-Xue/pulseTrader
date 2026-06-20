# pulseTrader — Project Memory

> Last updated: 2026-06-19
> 文件大小：5932 字符 / 20000 字符。更新本文件后必须重新计算并同步这一行。
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

## Current State (M12 Done, 2026-06-19)

### Test Summary
- **497 tests** (WEBUI + SQLITE): core 15 + config_loader 27 + config_validator 31 + logger 8 + exchange 59 + market 38 + execution 26 + risk 104 + strategy 59 + AI 43 + heartbeat 7 + webui 57 + trade_recorder 27
- 470 without SQLITE · 432 without WEBUI or SQLITE

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

### Next: Testnet + Paper Trading
- Gate.io testnet API setup (separate API keys)
- Paper trading mode (simulate fills without real orders)
- Integration testing with live futures market data
- Performance benchmarking (latency, throughput)

Then: 小资金实盘 → production hardening

## Config Structure

Key files: `src/core/config.hpp` (all structs), `config_loader.cpp` (TOML→struct), `config_validator.cpp` (semantic rules)

```
PulseConfig
├── ExchangeConfig   (apiKey, apiSecret, restBaseUrl, wsUrl, futuresWsUrl, proxyUrl)
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
- **.env**: `GATE_API_KEY`, `GATE_API_SECRET`, `HTTP_PROXY`, `HTTPS_PROXY` (→ `127.0.0.1:7897`)
- **Git proxy**: `http.proxy` / `https.proxy` = `http://127.0.0.1:7897`
- ⚠️ **Mainnet** — real money at risk

## Code Conventions

- `.clang-format`: Allman braces, 120 col, 4-space indent
- Naming: PascalCase classes, snake_case functions/vars, kPascalCase constants, trailing_underscore_ privates
- Yoda conditions, mandatory braces, `Result<T>` = `std::variant<T, PulseError>`
- `ExchangeConfig.restBaseUrl` = host only (`https://api.gateio.ws`), path includes `/api/v4`

## Notes

- QuantX (`~/1_Code/QuantX`) has reusable Gate.io code (signing, REST, futures adapter)
- Sub-account recommended for risk isolation (max 10 for VIP0-4, inherit main VIP)
- Futures: USDT-settled only, leverage up to 125x, simultaneous spot+futures via config
