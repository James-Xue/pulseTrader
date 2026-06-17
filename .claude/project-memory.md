# pulseTrader — Project Memory

> Last updated: 2026-06-17
> 文件大小：12895 字符 / 14000 字符。更新本文件后必须重新计算并同步这一行。

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

## Current State (2026-06-17)

### Completed
- **L2 Logging** (2026-06-15): spdlog async logger, per-module isolation, `PULSE_LOG_*` macros, 8 tests
- **L1 Exchange REST** (2026-06-16/17): Gate.io v4 REST client adapted from QuantX `gate_client`
  - `gate_auth`: SHA-512, HMAC-SHA512, `sign_request()` — pure stateless functions (OpenSSL)
  - `gate_rest_client`: libcurl + signing + retry (exponential backoff) + timeout + HTTP proxy auto-detect
  - Proxy: reads `ExchangeConfig.proxyUrl` or `HTTPS_PROXY`/`HTTP_PROXY` env vars, `CURLOPT_HTTPPROXYTUNNEL`
  - Default timeout: 10s (increased from 5s for proxy latency)
  - Public endpoints: `get_currencies()`, `get_currency_pairs()`, `get_ticker()`
  - Authenticated endpoint: `get_spot_accounts()`
  - Generic `request()` method for future expansion
  - 11 unit tests (NIST/RFC test vectors), smoke test tool
- **L1 Exchange WebSocket** (2026-06-16/17): Gate.io v4 WebSocket client with websocketpp + asio
  - `gate_ws_channels`: channel subscription/unsubscription, message dispatch by channel type
  - `gate_ws_client`: auto-reconnect with exponential backoff + jitter, ping/pong keepalive
  - Private channel HMAC authentication, connection state management
  - Proxy tunnel: `ProxyTunnel` class — local TCP forwarder + HTTP CONNECT tunnel + bidirectional relay
    - websocketpp connects to `wss://127.0.0.1:LOCAL_PORT/` (random port)
    - ProxyTunnel establishes CONNECT tunnel through HTTP proxy to real WSS server
    - TLS: `verify_none` for proxy mode (cert hostname mismatch: 127.0.0.1 vs real host)
  - Helper functions: `detect_proxy_url()`, `parse_ws_url()`
  - 24 unit tests (12 channels + 12 client), smoke test tool
- **L3 Market Data** (2026-06-16): Hot path market data pipeline
  - `ticker_cache`: thread-safe storage for latest ticker per symbol (shared_mutex)
  - `symbol_registry`: REST-fetched instrument metadata (tick size, lot size, min notional)
  - `kline_buffer`: fixed-size ring buffer with seqlock pattern for lock-free reads
  - `orderbook_manager`: snapshot + delta incremental updates, sequence validation, gap detection
  - `market_feed`: dispatcher that wires WS events to L3 components
  - 32 unit tests, smoke test tool
- **L8 Order Execution** (2026-06-16): Order lifecycle management
  - `execution_report`: immutable fill record with slippage/fees/latency metrics
  - `order_executor`: REST order placement (market/limit/post-only) with retry logic
  - `order_tracker`: WS private channel + REST polling fallback, state machine, ExecutionReport generation
  - 22 unit tests, smoke test tool
- **L7 Risk Management** (2026-06-16): Safety gate between Strategy (L6) and Execution (L8)
  - `risk_types`: shared types — `RiskDecision` enum, `RiskEvalResult`, `Position`, `PortfolioSummary`
  - `position_manager`: thread-safe position tracking (shared_mutex), portfolio/symbol notional limits
  - `drawdown_guard`: rolling PnL monitor, daily/peak-to-valley circuit breaker (atomic halt flag)
  - `order_rate_limiter`: lock-free token-bucket rate limiter (atomic + CAS loop)
  - `risk_manager`: central order gate — evaluate_order() returns Approved/Modified/Rejected
  - `stop_loss_engine`: fixed/trailing/time-based stop modes, pure evaluator (no execution)
  - `take_profit_engine`: partial take-profit ladders with N targets, pure evaluator
  - Config: `StopMode`, `StopLossConfig`, `TakeProfitConfig` added to config.hpp
  - Error codes: `RateLimitHit=3003`, `StopLossTriggered=3004`, `TakeProfitTriggered=3005`, `SymbolLimitHit=3006`
  - 92 unit tests
- **L6 Strategy Engine** (2026-06-16): Automatic signal generation with multi-strategy orchestration
  - `signal_types`: `SignalType` enum (Buy/Sell/Flat), `TradingSignal` struct with confidence/price/reason
  - `strategy_params`: hot-reloadable parameters via `std::atomic<double>` (AI-layer ready, lock-free)
  - `strategy_context`: dependency injection bundle (MarketFeed, RiskManager, OrderExecutor, config)
  - `strategy_base`: abstract base class with lifecycle hooks (on_tick, on_kline, on_orderbook), signal emission
  - `strategy_manager`: multi-strategy orchestration, one `std::jthread` per strategy, `std::stop_token` cancellation
  - `momentum_scalper`: EMA crossover strategy (fast/slow EMA, crossover detection, confidence from EMA distance)
  - `orderbook_scalper`: order book imbalance strategy (bid/ask volume ratio, threshold-based signals)
  - `mean_reversion_scalper`: Bollinger Band mean-reversion strategy (SMA + stddev bands, oversold/overbought)
  - `signal_aggregator`: weighted voting across strategies, per-symbol cooldown, threshold-based emission
  - Config: `StrategyInstanceConfig` (per-strategy name/symbol/quantity/confidence), `StrategyConfig` (aggregator threshold/cooldown)
  - 52 unit tests, smoke test tool (`tools/test_strategy.cpp`)
- **L4 AI Analysis** (2026-06-17): LLM-driven parameter adaptation
  - `analysis_result`: Sentiment/Volatility enums, ParamDeltas (10 deltas 1:1 to StrategyParams), JSON ADL
  - `twitter_feed`: X API v2 polling, rolling deque with ID dedup, `enabled=false` by default
  - `news_feed`: NewsAPI/CryptoPanic dual-provider, URL dedup, `enabled=false` by default
  - `prompt_builder`: Fixed system prompt enforcing JSON schema, dynamic user prompt
  - `ai_client`: OpenAI/Claude dual-backend, injectable HttpTransport for testing, retry
  - `param_advisor`: Safety-bounded deltas (max_delta + hard bounds clamp), atomic writes
  - `ai_pipeline`: Full-cycle orchestrator, each step tolerates failure independently
  - Config: `TwitterConfig`, `NewsConfig`, `AiConfig.baseUrl/maxRetries`; Error codes: 4002-4004
  - StrategyParams: added `stop_loss_pct`, `take_profit_pct`; 43 tests, smoke tool (`--mock`)
- **L5 Heartbeat Scheduler** (2026-06-17): 5-min AI analysis clock
  - `task_queue`: Priority queue + worker jthread, exception-safe
  - `heartbeat_scheduler`: asio::steady_timer, drift-free re-arm, manual trigger
  - 7 unit tests
- **L9 WebUI Dashboard** (2026-06-17): Browser-based real-time monitoring
  - `snapshot_types`: 8 per-panel snapshot structs + nlohmann JSON ADL serialization
  - `dashboard_state`: Tiered polling aggregator (200ms/500ms/1s/60s), std::jthread with stop_token
  - `web_server`: uWebSockets HTTP server, static SPA serving, REST API (/api/status, /api/snapshot), bearer token auth, Host header validation, PIMPL pattern
  - `ws_server`: WebSocket push server, client tracking, JSON broadcast, max client enforcement
  - Frontend: vanilla HTML/CSS/JS dark-theme dashboard, 8 panels, WS auto-reconnect
  - Interface gap bridges: TickerCache::symbols(), SymbolRegistry::symbols(), StrategyManager::snapshot(), AiPipeline::last_result(), OrderTracker::active_orders()+recent_reports(), RiskManager::risk_snapshot()
  - Config: gated by `-DPULSE_ENABLE_WEBUI=ON`, error codes 9100-9105
  - 57 unit tests, smoke test tool (`tools/test_webui.cpp`)
- **Coding standards** (2026-06-15/16): AGENTS.md with Allman brace style, Yoda conditions, mandatory braces, English-only, detailed comments

### Test Summary
- 357 tests total (with WEBUI): core 9 + logger 8 + exchange 35 + market 32 + execution 22 + risk 92 + strategy 52 + AI 43 + heartbeat 7 + webui 57 — all passing
- 319 tests (without WEBUI): same minus webui — all passing

### Milestones Achieved
- **M1** ✅: End-to-end Exchange → Market Data → Execution pipeline
- **M2** ✅: Automatic trading: Market Data → Strategy → Risk → Execution
- **M3** ✅: AI adaptive — strategy parameters auto-tune every 5 min via LLM analysis
- **M4** ✅: Complete product — all 9 layers operational, WebUI dashboard with real-time monitoring

### Next Steps (per roadmap)
- All phases complete — Milestone M4 achieved. Future enhancements: MetricsCollector (L2), config file loading (TOML), SQLite trade recorder, TLS support, strategy parameter tuning via WebUI.

### Operational Setup (2026-06-17)
- **Branch status**: All feature branches merged into `main` and deleted (local + remote). Only `main` branch exists.
- **run.sh**: Convenience script in project root — `./run.sh {rest|ws|market|strategy|ai|webui|test}`
  - Auto-sources `.env` for API credentials and proxy settings
  - Commands: rest (REST API test), ws (WebSocket test), market, strategy, ai (mock), webui, test (357 unit tests)
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

## Notes

- Project originally at `~/1_Code/pulseTrader`, moved to `~/1_Code/09_pulseTrader`
- QuantX (`~/1_Code/QuantX`) has reusable Gate.io code (signing, REST client, futures adapter)
- Sub-account API trading recommended for risk isolation (one sub-account per strategy)
- Gate.io sub-accounts: inherit main VIP, max 10 (VIP0-4) or 30 (VIP5-9), cannot delete once created
