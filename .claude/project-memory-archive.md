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
