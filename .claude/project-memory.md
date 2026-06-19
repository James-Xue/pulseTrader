# pulseTrader — Project Memory

> Last updated: 2026-06-19
> 文件大小：4896 字符 / 20000 字符。更新本文件后必须重新计算并同步这一行。
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

## Current State (M9 Done, 2026-06-19)

### Test Summary
- **467 tests** (WEBUI + SQLITE): core 15 + config_loader 27 + config_validator 31 + logger 8 + exchange 53 + market 33 + execution 22 + risk 92 + strategy 52 + AI 43 + heartbeat 7 + webui 57 + trade_recorder 27
- 440 without SQLITE · 402 without WEBUI or SQLITE

### Milestones
- **M1–M5** ✅: Core pipeline → strategy → risk → AI → WebUI → trading engine
- **M6** ✅: TOML config (`--config trading.toml`, `from_env:` syntax)
- **M7** ✅: SQLite trade recorder (17-col schema, 4 queries)
- **M8** ✅: Futures config foundation (MarketType/MarginMode enums, 7xxx errors)
- **M9** ✅: EndpointRouter + WS ping/pong fix
  - `src/exchange/endpoint_router.hpp/cpp` — `MarketType` → REST paths / WS prefixes / ping-pong channels
  - `GateWsClient(config, MarketType=Spot)` — URL via `EndpointRouter::select_ws_url()`, ping/pong generalized
  - `GateWsChannels::build_pong(frame, MarketType)` — dynamic `.ping`→`.pong` derivation
  - `GateRestClient(config, MarketType=Spot)` — +3 futures methods (contracts/ticker/accounts)
  - 18 tests (13 EndpointRouter + 3 build_pong + 2 constructor compat)

### Next: M10 — Futures Market Data
- futures ticker / mark_price / funding_rate parsing
- `SymbolInfo.contract_multiplier` field
- dual MarketFeed (spot + futures)
- ~14 new tests

Then M11 (futures risk/PnL) → M12 (futures execution + dual-market wiring) → testnet → paper trading → 小资金实盘

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
