# pulseTrader — Implementation Roadmap

> **Version:** 0.1.0-dev\
> **Created:** 2026-06-15\
> **Strategy:** Vertical slice first — connect exchange → receive market data → place an order\
> Then expand horizontally with risk, strategy, AI, and WebUI.

---

## Principle

**First, cut through the narrowest vertical path end-to-end, then expand horizontally.**

Vertical path = Exchange → Market Data → Execution — can be debugged with curl, receives real market data, and can place a test order. Once this chain works, the system's "skeleton" is established, and every subsequent layer adds muscle to the skeleton.

---

## Phase 1 — Foundation (Layer 2 + Layer 1)

> **Goal**: Compile successfully + connect to Gate.io + retrieve account info and market data

### Step 1.1: Layer 2 — Logging

| Item | Detail |
|------|--------|
| Files | `logger.hpp / .cpp` |
| Scope | spdlog async logger wrapper, per-module named loggers |
| Key work | `PULSE_LOG_INFO/WARN/ERROR` macros, async sink + bounded queue |
| Test | Unit tests: log level filtering, module isolation, async non-blocking for callers |
| Why first | All subsequent layers need logging — building it first is like sharpening the axe |

**Deliverable**: `tests/unit/test_logger.cpp` passes, `cmake --build` full compilation succeeds

### Step 1.2: Layer 1 — Exchange (REST)

| Item | Detail |
|------|--------|
| Files | `gate_auth.hpp / .cpp`, `gate_rest_client.hpp / .cpp` |
| Scope | HMAC-SHA512 signing, libcurl wrapper, rate limit handling, JSON deserialization |
| Key work | First implement `GET /api/v4/spot/currencies` (public endpoint, signature verification without API key) → then implement `GET /api/v4/spot/accounts` (private endpoint, verify signature) |
| Test | Integration tests: signature vector comparison, mock HTTP server for retry/backoff |
| Depends on | Logger (L2) |

**Deliverable**: `tools/test_gate_rest.cpp` successfully fetches trading pair list and account balances

### Step 1.3: Layer 1 — Exchange (WebSocket)

| Item | Detail |
|------|--------|
| Files | `gate_ws_client.hpp / .cpp`, `gate_ws_channels.hpp / .cpp` |
| Scope | websocketpp + asio persistent connection, auto-reconnect (exponential backoff + jitter), heartbeat ping, channel subscribe/dispatch |
| Key work | First subscribe to `spot.tickers` public channel to verify connection → then subscribe to private channels to verify WS signing |
| Test | Integration test: connect → receive ticker → disconnect & reconnect → receive ticker again |
| Depends on | REST client (signature verification logic reuse), Logger (L2) |

**Deliverable**: `tools/test_gate_ws.cpp` subscribes to ticker channel and continuously prints real-time prices

---

## Phase 2 — Market Data Pipeline (Layer 3) ✅ COMPLETED

> **Goal**: Receive structured real-time market data that the strategy layer can consume directly
> **Status**: ✅ Done (2026-06-16) — 32 unit tests, smoke test tool, all 84 tests passing

### Step 2.1: TickerCache + SymbolRegistry ✅

| Item | Detail |
|------|--------|
| Files | `ticker_cache.hpp / .cpp`, `symbol_registry.hpp / .cpp` |
| Scope | Thread-safe storage (shared_mutex) for latest ticker; REST fetch instrument metadata (tick size, lot size, min notional) |
| Test | 12 unit tests: concurrent updates, symbol lookup, order validation |
| Notes | TickerCache uses shared_mutex (not atomic) due to Ticker struct size; SymbolRegistry validates order params against metadata |

### Step 2.2: OrderBookManager ✅

| Item | Detail |
|------|--------|
| Files | `orderbook_manager.hpp / .cpp` |
| Scope | snapshot + delta incremental updates, sequence number validation, gap detection triggers re-subscription |
| Test | 11 unit tests: snapshot init, delta apply, sequence gap, top N bids/asks |
| Notes | Uses std::map for sorted price levels; resubscribe callback on sequence gap |

### Step 2.3: KlineBuffer ✅

| Item | Detail |
|------|--------|
| Files | `kline_buffer.hpp / .cpp` |
| Scope | Fixed-size ring buffer (500 candles), seqlock pattern for lock-free snapshot reads |
| Test | 12 unit tests: ring wrap-around, concurrent push/snapshot, seqlock consistency |
| Notes | Seqlock ensures readers see consistent snapshots without locks; per-symbol buffers in MarketFeed |

### Step 2.4: MarketFeed Dispatcher ✅

| Item | Detail |
|------|--------|
| Files | `market_feed.hpp / .cpp` |
| Scope | Integrates all L3 components, subscribes to Gate.io WS channels (tickers, order_book, candlesticks), routes events |
| Test | Smoke test `tools/test_market_feed.cpp` connects to Gate.io, prints BTC_USDT ticker + orderbook top 5 + K-line |
| Notes | No separate dispatch thread — callbacks execute on WS I/O thread; per-symbol KlineBuffer map |

**Deliverable**: ✅ `tools/test_market_feed.cpp` prints real-time BTC_USDT ticker + orderbook top 5 + K-line closing prices

---

## Phase 3 — Order Execution (Layer 8) ✅ COMPLETED

> **Goal**: Trigger order placement manually, verify end-to-end pipeline
> **Status**: ✅ Done (2026-06-16) — 22 unit tests, smoke test tool, all 106 tests passing
> **Milestone M1**: ✅ End-to-end Exchange → Market Data → Execution pipeline achieved

### Step 3.1: OrderExecutor ✅

| Item | Detail |
|------|--------|
| Files | `order_executor.hpp / .cpp` |
| Scope | REST order placement (market/limit/post-only), retry logic for transient failures |
| Test | Integration test: testnet order placement, response parsing |
| Notes | Uses `Result<OrderResponse>` for place_order, `bool` for cancel_order (simpler than Result<void>) |

### Step 3.2: OrderTracker ✅

| Item | Detail |
|------|--------|
| Files | `order_tracker.hpp / .cpp` |
| Scope | WS private channel (spot.orders) subscription, REST polling fallback, state machine, ExecutionReport generation |
| Test | Unit tests: state machine transitions, status parsing; Integration test: full order lifecycle tracking |
| Notes | Completion callback invoked on terminal state; slippage calculated vs mid-price at submission |

### Step 3.3: ExecutionReport ✅

| Item | Detail |
|------|--------|
| Files | `execution_report.hpp / .cpp` |
| Scope | Immutable fill record: order_id, symbol, side, qty, fill_price, slippage (bps), fees, latency |
| Test | Unit tests: construction, to_json() serialization, slippage calculation |
| Notes | Slippage formula: (fill_price - mid_price) / mid_price * 10000; inverted for Sell orders |

**Deliverable**: ✅ `tools/test_execution.cpp` places limit order on testnet → tracks via WS → prints ExecutionReport

> ✅ **Milestone M1**: End-to-end pipeline operational. `Exchange → Market Data → Execution` runs, fetches market data and places orders.

---

## Phase 4 — Risk Management (Layer 7) ✅ COMPLETED

> **Goal**: Add a safety gate before order placement
> **Status**: ✅ Done (2026-06-16) — 92 unit tests, all 198 tests passing
> **Branch**: `feat/layer7-risk-management` (merged)

### Step 4.1: Foundation — risk_types + PositionManager ✅

| Item | Detail |
|------|--------|
| Files | `risk_types.hpp`, `position_manager.hpp / .cpp` |
| Scope | Shared types (RiskDecision, RiskEvalResult, Position, PortfolioSummary) + thread-safe position tracking (shared_mutex), portfolio/symbol notional limits |
| Config | New `StopMode` enum, `StopLossConfig`, `TakeProfitConfig`; `RiskConfig` adds `maxSymbolNotional` |
| Error codes | `RateLimitHit(3003)`, `StopLossTriggered(3004)`, `TakeProfitTriggered(3005)`, `SymbolLimitHit(3006)` |
| Test | 23 unit tests: open/close/limits/queries/aggregation/thread safety |

### Step 4.2: DrawdownGuard + OrderRateLimiter ✅

| Item | Detail |
|------|--------|
| Files | `drawdown_guard.hpp / .cpp`, `order_rate_limiter.hpp / .cpp` |
| Scope | Rolling PnL monitoring + intraday/peak drawdown circuit breaker (atomic halt flag); lock-free token-bucket rate limiting (atomic + CAS loop) |
| Test | 26 unit tests (14 + 12): equity tracking, drawdown triggers, token acquire/refill, thread safety |

### Step 4.3: RiskManager Orchestrator ✅

| Item | Detail |
|------|--------|
| Files | `risk_manager.hpp / .cpp` |
| Scope | Central order approval gateway: `evaluate_order(OrderRequest)` → Approved / Modified(reduced qty) / Rejected(reason code) |
| Flow | DrawdownGuard halt check → OrderRateLimiter token check → PositionManager limit check |
| Test | 15 unit tests: approve/reject/modify across all rules, halt-clear recovery |

### Step 4.4: StopLossEngine + TakeProfitEngine ✅

| Item | Detail |
|------|--------|
| Files | `stop_loss_engine.hpp / .cpp`, `take_profit_engine.hpp / .cpp` |
| Scope | Three-mode stop loss (Fixed/Trailing/TimeBased) + ladder take profit (N targets + fractions), pure evaluator without order execution |
| Test | 28 unit tests (16 + 12): fixed/trailing/time stops, ladder progression, multi-position tracking |

**Deliverable**: ✅ 92 unit tests with full coverage, `pulse::risk` static library compiles successfully

---

## Phase 5 — Strategy Engine (Layer 6) ✅ COMPLETED

> **Goal**: Automatically generate trading signals, replacing manual order placement
> **Status**: ✅ Done (2026-06-16) — 52 unit tests, smoke test tool, all 250 tests passing
> **Branch**: `feat/layer6-strategy-engine`

### Step 5.1: Strategy Infrastructure ✅

| Item | Detail |
|------|--------|
| Files | `signal_types.hpp`, `strategy_params.hpp`, `strategy_context.hpp`, `strategy_base.hpp`, `strategy_manager.hpp / .cpp` |
| Scope | SignalType enum + TradingSignal struct; atomic hot-reload parameters; DI context injection; abstract base class + lifecycle hooks; multi-strategy jthread orchestration + stop_token cancellation |
| Config | New `StrategyInstanceConfig` (per-strategy name/symbol/quantity/confidence) and `StrategyConfig` (aggregator threshold/cooldown) added to `PulseConfig` |
| Test | 20 unit tests: signal defaults, atomic read/write, concurrent access, base class interface, manager lifecycle |

### Step 5.2: MomentumScalper ✅

| Item | Detail |
|------|--------|
| Files | `momentum_scalper.hpp / .cpp` |
| Scope | EMA crossover trend-following strategy: fast EMA / slow EMA crossover detection, confidence normalized from EMA distance |
| Test | 7 unit tests: name/id, default params, on_tick/on_orderbook ignored, insufficient data, hot-reload |

### Step 5.3: OrderBookScalper + MeanReversionScalper ✅

| Item | Detail |
|------|--------|
| Files | `orderbook_scalper.hpp / .cpp`, `mean_reversion_scalper.hpp / .cpp` |
| Scope | Order book imbalance strategy (bid/ask volume ratio + threshold); Bollinger Band mean reversion strategy (SMA + stddev bands + overbought/oversold detection) |
| Test | 17 unit tests (9 + 8): imbalance buy/sell signals, balanced book, depth check, cooldown, Bollinger params |

### Step 5.4: SignalAggregator ✅

| Item | Detail |
|------|--------|
| Files | `signal_aggregator.hpp / .cpp` |
| Scope | Multi-strategy weighted voting, per-symbol cooldown, threshold-triggered aggregated signal output; strategy weights dynamically adjustable (reserved for AI layer) |
| Test | 11 unit tests: flat ignored, threshold, weighted signals, buy/sell dominance, cooldown, different symbols, reset |

**Deliverable**: ✅ `tools/test_strategy.cpp` validates all 3 strategies + SignalAggregator + StrategyManager lifecycle

> ✅ **Milestone M2**: Automated trading. `Market Data → Strategy → Risk → Execution` fully automated closed loop.

---

## Phase 6 — AI Pipeline (Layer 5 + Layer 4) ✅ COMPLETED

> **Goal**: Integrate LLM for adaptive parameter tuning
> **Status**: ✅ Done (2026-06-17) — 50 unit tests, smoke test tool, all 300 tests passing
> **Branch**: `feat/layer6-strategy-engine`

### Step 6.1: AI Analysis (Layer 4) ✅

| Item | Detail |
|------|--------|
| Files | `twitter_feed`, `news_feed`, `prompt_builder`, `ai_client`, `analysis_result`, `param_advisor`, `ai_pipeline` |
| Scope | Social media/news collection → prompt assembly → LLM invocation → JSON schema validation → parameter delta application |
| Notes | HttpTransport injection for testability; social feeds disabled by default; 10-delta ParamDeltas mapping 1:1 to StrategyParams |

### Step 6.2: Heartbeat Scheduler (Layer 5) ✅

| Item | Detail |
|------|--------|
| Files | `heartbeat_scheduler`, `task_queue`, `heartbeat_events` |
| Scope | asio::steady_timer 5-minute heartbeat → TaskQueue → full AI pipeline chain |
| Notes | Single worker jthread; exception-safe task execution; drift-free timer re-arm |

**Deliverable**: ✅ `tools/test_ai_pipeline.cpp` simulates one full heartbeat cycle, verifies parameter updates

> ✅ **Milestone M3**: AI self-adaptation. Strategy parameters automatically adjusted every 5 minutes based on market sentiment.

---

## Phase 7 — WebUI Dashboard (Layer 9) ✅ COMPLETED

> **Goal**: Real-time browser monitoring — the cherry on top

### Step 7.1: DashboardState + Snapshot Types

| Item | Detail |
|------|--------|
| Files | `dashboard_state.hpp / .cpp`, `snapshot_types.hpp` |
| Scope | Layered polling threads, per-layer snapshot data structures |

### Step 7.2: WebServer + WsServer

| Item | Detail |
|------|--------|
| Files | `web_server.hpp / .cpp`, `ws_server.hpp / .cpp` |
| Scope | uWebSockets HTTP (static SPA) + WS (real-time push), bearer token auth, Host header validation |

### Step 7.3: Frontend SPA

| Item | Detail |
|------|--------|
| Scope | Order book depth chart, K-line + signal markers, position/order/PnL/AI analysis cards |

**Deliverable**: `-DPULSE_ENABLE_WEBUI=ON` compile flag enables full dashboard access via browser

> ✅ **Milestone M4**: Complete product. All 9 layers operational, ready for public release.

---

## Phase 9 — Trading Engine (apps/pulsetrader)

> ✅ **Completed** — `apps/pulsetrader/main.cpp` (9-layer integration), `run.sh trade`, WS JSON fix, operations guide
> ✅ **Milestone M5**: Trading Engine — deployable complete trading system

## Phase 10 — TOML Config Loader

> ✅ **Completed** — `config_loader` + `config_validator` + `trading.toml.example`, toml11 v4, 46 tests
> ✅ **Milestone M6**: File-driven configuration — `--config trading.toml`

## Phase 11 — SQLite Trade Recorder

> ✅ **Completed** — `trade_recorder`, 17 columns, 4 query APIs, 27 tests
> ✅ **Milestone M7**: SQLite persistent trade records

## Phase 12 — Futures Config Foundation (M8)

> ✅ **Completed** — MarketType/MarginMode enums, futures config fields, 7xxx error codes, 18 tests
> ✅ **Milestone M8**: Futures config foundation — types/configuration/validation triple ready

## Phase 13 — Futures Endpoint Router + WS Ping Fix (M9)

> ✅ **Completed** — EndpointRouter pure-function routing + WS ping/pong generalization + futures REST convenience methods, 18 tests

| Item | Detail |
|------|--------|
| Files | NEW `endpoint_router.hpp/.cpp`, MODIFY `gate_ws_client.cpp`, `gate_ws_channels.cpp`, `gate_rest_client.cpp` |
| Scope | Pure-function routing (MarketType → REST paths / WS channel prefixes / ping-pong channels) + WS ping/pong generalization |
| Key work | EndpointRouter::rest_prefix/ws_channel/ping_channel/pong_channel/select_ws_url/needs_json_ping |
| WS fix | Spot: JSON spot.ping/spot.pong; Futures: RFC 6455 (websocketpp handles automatically) + JSON compatible |
| REST | New get_futures_contracts/get_futures_ticker/get_futures_accounts |
| Test | 18 tests: EndpointRouter×13, WS ping/pong×3, constructor compatibility×2 |

## Phase 14 — Futures Market Data (M10)

> ✅ **Completed** — Ticker/SymbolInfo futures fields, dual MarketFeed, EndpointRouter order routing, 11 tests

| Item | Detail |
|------|--------|
| Files | `ticker_cache.hpp`, `symbol_registry.hpp/.cpp`, `market_feed.hpp/.cpp`, `endpoint_router.hpp/.cpp`, `gate_rest_client.hpp/.cpp` |
| Scope | Ticker adds mark_price/index_price/funding_rate |
| | SymbolInfo adds quanto_multiplier/leverage_max/min/maintenance_rate/funding_interval/order_size_min/max/market_type |
| | MarketFeed constructor accepts MarketType, channel prefix parameterized, dual-format JSON parsing |
| | EndpointRouter adds orders_path/order_path/leverage_path |
| | GateRestClient adds post/cancel/get_futures_order |
| Test | 11 tests: EndpointRouter×6, TickerCache×2, SymbolRegistry×3 |

## Phase 15 — Futures Risk & PnL (M11)

> ✅ **Completed** — Leverage-aware PnL, futures position management, futures risk checks, 12 tests

| Item | Detail |
|------|--------|
| Files | `risk_types.hpp`, `position_manager.hpp/.cpp`, `risk_manager.hpp/.cpp` |
| Scope | Position adds leverage/margin_mode/margin_used/liquidation_price/quanto_multiplier/market_type |
| PnL | Unified formula: `direction × (current - entry) × qty × quanto_multiplier × leverage` (spot defaults=1.0) |
| Margin | `qty × entry × quanto / leverage`, new evaluate_futures_order() for leverage/margin checks |
| | PortfolioSummary adds total_margin_used/futures_position_count |
| Test | 12 tests: PositionManager×8, RiskManager×4 |

## Phase 16 — Futures Execution + Dual-Market Wiring (M12)

> ✅ **Completed** — Futures order execution, dual-market infrastructure wiring, strategy market routing, 7 tests

| Item | Detail |
|------|--------|
| Files | `order_executor.hpp/.cpp`, `order_tracker.hpp/.cpp`, `signal_types.hpp`, `strategy_manager.cpp`, `main.cpp` |
| Scope | OrderRequest adds market_type/leverage/reduce_only/contract_size |
| Orders | Spot: currency_pair/side/amount; Futures: contract/signed size/reduce_only/tif |
| Tracker | WS channel parameterized (spot.orders vs futures.orders), REST path routing, dual-format response parsing (int id, finish_as) |
| Signal | TradingSignal adds market_type, emit_signal() auto-fills strategy market_type |
| main.cpp | Creates dual-market infrastructure on demand (REST/WS/Feed/Executor/Tracker), strategies routed by market_type |
| Test | 7 tests: OrderRequest×4, TradingSignal×3 |

---

## Dependency Graph

```
L2 (Logging) ─────────────────────────────────────────► all layers
       │
L1 (Exchange) ──► L3 (Market Data) ──► L6 (Strategy) ──► L7 (Risk) ──► L8 (Execution)
       │                                    ▲                │
       │                                    │                │
       └────────────────────────────────────┘                │
                      L5 (Heartbeat) ──► L4 (AI) ──► ParamAdvisor ──► L6
                                                              │
                                              L9 (WebUI) ◄───┘  (reads from all)
```

## CMake Feature Flags

| Flag | Default | Gates |
|------|---------|-------|
| `PULSE_ENABLE_SQLITE` | OFF | SQLiteCpp for TradeRecorder |
| `PULSE_ENABLE_TOML` | OFF | toml11 for config file |
| `PULSE_ENABLE_WEBUI` | OFF | uWebSockets for dashboard |

## File Naming Convention

- Headers: `include/pulse/<layer>/<module>.hpp`
- Sources: `src/<layer>/<module>.cpp`
- Tests: `tests/unit/test_<module>.cpp` / `tests/integration/test_<flow>.cpp`
- Tools: `tools/test_<feature>.cpp` (manual smoke tests, not in CTest)
