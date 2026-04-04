# pulseTrader — Architecture

> **Version:** 0.1.0-dev  
> **Language:** C++20  
> **Exchange:** Gate.io (REST + WebSocket)  
> **Namespace:** `pulse::`  
> **Build system:** CMake + vcpkg

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [High-Level Architecture](#2-high-level-architecture)
3. [Module Reference](#3-module-reference)
   - [Layer 1 — Exchange](#layer-1--exchange)
   - [Layer 2 — Market Data](#layer-2--market-data)
   - [Layer 3 — Strategy Engine](#layer-3--strategy-engine)
   - [Layer 4 — Heartbeat Scheduler](#layer-4--heartbeat-scheduler)
   - [Layer 5 — AI Analysis](#layer-5--ai-analysis)
   - [Layer 6 — Risk Management](#layer-6--risk-management)
   - [Layer 7 — Order Execution](#layer-7--order-execution)
   - [Layer 8 — Logging & Monitoring](#layer-8--logging--monitoring)
4. [Data Flow](#4-data-flow)
5. [Threading Model](#5-threading-model)
6. [Key Design Decisions](#6-key-design-decisions)
7. [Third-Party Dependencies](#7-third-party-dependencies)

---

## 1. Project Overview

pulseTrader is a C++20 high-frequency scalping framework that integrates real-time market data, AI-driven sentiment analysis, and adaptive parameter tuning into a single cohesive system. It targets the Gate.io spot and futures markets exclusively, favouring depth of integration over breadth of exchange support.

The system is structured as eight vertical layers. Each layer has a single well-defined responsibility and communicates with adjacent layers through narrow, typed interfaces. This design keeps the hot market-data path free of latency spikes caused by AI inference, disk I/O, or network calls.

---

## 2. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          pulseTrader Process                            │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  Layer 4: HeartbeatScheduler  (every 5 min)                      │   │
│  │    └─► TaskQueue ──► AIAnalyzer ──► ParamAdvisor                 │   │
│  └──────────────────────────────┬───────────────────────────────────┘   │
│                                 │ param updates (atomic writes)         │
│  ┌──────────────────────────────▼───────────────────────────────────┐   │
│  │  Layer 3: Strategy Engine                                        │   │
│  │    MomentumScalper  │  OrderBookScalper  │  MeanReversionScalper │   │
│  │    SignalAggregator (weighted voting)                            │   │
│  └──────────────────────────────┬───────────────────────────────────┘   │
│                                 │ signals                               │
│  ┌──────────────────────────────▼───────────────────────────────────┐   │
│  │  Layer 6: Risk Management                                        │   │
│  │    RiskManager │ PositionManager │ StopLoss/TakeProfit Engines   │   │
│  └──────────────────────────────┬───────────────────────────────────┘   │
│                                 │ approved orders                       │
│  ┌──────────────────────────────▼───────────────────────────────────┐   │
│  │  Layer 7: Order Execution                                        │   │
│  │    OrderExecutor │ OrderTracker │ ExecutionReport                │   │
│  └──────────────────────────────┬───────────────────────────────────┘   │
│                                 │                                       │
│  ┌──────────────────────────────▼───────────────────────────────────┐   │
│  │  Layer 8: Logging & Monitoring                                   │   │
│  │    Logger │ TradeRecorder │ MetricsCollector │ AlertManager      │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  Layer 2: Market Data  (hot path — dedicated thread)             │   │
│  │    MarketFeed │ OrderBookManager │ KlineBuffer │ TickerCache     │   │
│  └──────────────────────────────┬───────────────────────────────────┘   │
│                                 │                                       │
│  ┌──────────────────────────────▼───────────────────────────────────┐   │
│  │  Layer 1: Exchange  (Gate.io REST + WebSocket)                   │   │
│  │    GateRestClient │ GateWsClient │ GateWsChannels │ GateAuth     │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Module Reference

### Layer 1 — Exchange

| Property | Value |
|---|---|
| Headers | `include/pulse/exchange/` |
| Sources | `src/exchange/` |

**Responsibility**

The Exchange layer is the sole point of contact with Gate.io. It abstracts all protocol-level details so that higher layers never need to construct HTTP signatures or handle WebSocket frames directly.

- **REST client** — Constructs signed HTTP requests using HMAC-SHA512. Handles rate limiting, retries with exponential back-off, and JSON deserialization of responses.
- **WebSocket client** — Maintains a persistent connection to the Gate.io WS gateway with automatic reconnection (exponential back-off, jitter) and heartbeat pings. Dispatches raw JSON frames to registered channel handlers.
- **Channel subscriptions** — Typed subscription helpers for each channel (order book, trades, tickers, K-lines, user orders). Each subscription maps to a concrete callback interface consumed by the Market Data layer.
- **Authentication** — Generates API key / secret based HMAC-SHA512 signatures for both REST and WebSocket private channels.

**Key files**

| File | Description |
|---|---|
| `gate_rest_client.hpp / .cpp` | Signed REST client (libcurl + OpenSSL) |
| `gate_ws_client.hpp / .cpp` | WebSocket client with auto-reconnect (websocketpp + asio) |
| `gate_ws_channels.hpp / .cpp` | Typed channel subscription registry |
| `gate_auth.hpp / .cpp` | HMAC-SHA512 request signing utilities |

---

### Layer 2 — Market Data

| Property | Value |
|---|---|
| Headers | `include/pulse/market/` |
| Sources | `src/market/` |

**Responsibility**

The Market Data layer receives raw frames from the Exchange layer and maintains authoritative, always-consistent in-memory views of the market that strategy threads can query with minimal latency.

- **Market feed dispatcher** — Routes incoming WebSocket events to the appropriate sub-component (order book, K-line, ticker). Runs on a single dedicated thread to avoid lock contention on the hot path.
- **Order book manager** — Applies incremental updates (snapshot + delta sequence) to maintain a full sorted order book. Validates sequence numbers and requests re-subscription on gaps.
- **K-line ring buffer** — Thread-safe circular buffer storing the N most recent candles per symbol/interval. Readers (strategy threads) hold no locks; they read via a snapshot copy-on-query pattern backed by a seqlock.
- **Ticker cache** — Lock-free cache of the latest ticker (best bid/ask, last price, 24h vol) per symbol, updated via `std::atomic` stores.
- **Symbol registry** — Central repository of instrument metadata (tick size, lot size, min notional, trading status) populated from REST at startup and refreshed periodically.

**Key files**

| File | Description |
|---|---|
| `market_feed.hpp / .cpp` | Event dispatcher and thread management |
| `orderbook_manager.hpp / .cpp` | Incremental order book reconstruction |
| `kline_buffer.hpp / .cpp` | Thread-safe ring buffer for candles |
| `ticker_cache.hpp / .cpp` | Lock-free latest ticker store |
| `symbol_registry.hpp / .cpp` | Instrument metadata and validation |

---

### Layer 3 — Strategy Engine

| Property | Value |
|---|---|
| Headers | `include/pulse/strategy/` |
| Sources | `src/strategy/` |

**Responsibility**

The Strategy Engine hosts one or more concurrent strategy instances, each running on its own `std::jthread`. It provides the lifecycle framework, context injection, and signal aggregation that turn market observations into trading signals.

- **Abstract strategy base** — Defines the strategy contract via pure-virtual lifecycle hooks:
  - `on_tick(const Ticker&)` — called on every best-bid/ask update
  - `on_orderbook(const OrderBook&)` — called when the top N levels change
  - `on_kline(const Kline&)` — called on candle close
  - `on_heartbeat(const HeartbeatEvent&)` — called every 5 minutes
  - `on_ai_update(const AnalysisResult&)` — called after each AI analysis cycle
- **Strategy manager** — Instantiates, starts, and stops strategies. Broadcasts market events to all running strategies. Applies `ParamAdvisor` deltas by writing to each strategy's `StrategyParams`.
- **Strategy context** — Dependency-injection bundle passed to each strategy at construction: market data views, risk manager handle, order executor handle, and logger.
- **Strategy params (hot-reload)** — `StrategyParams` stores key tunable values as `std::atomic<double>` fields. `ParamAdvisor` writes updated values after each AI cycle; strategy threads read atomically without acquiring any lock.

**Built-in scalping strategies**

| Class | Algorithm |
|---|---|
| `MomentumScalper` | Detects short-term momentum via EMA crossover on K-lines; enters in the direction of the cross, exits on reversal or stop-loss. |
| `OrderBookScalper` | Analyses order book imbalance (bid/ask volume ratios at top N levels) to predict short-term price direction; places limit orders at optimal queue position. |
| `MeanReversionScalper` | Tracks Bollinger Band deviations on tick data; fades extremes with a tight profit target and stop-loss. |

**Signal aggregator**

When multiple strategies are active, the `SignalAggregator` collects individual buy/sell/flat signals, weights them by per-strategy confidence scores (updated dynamically by the AI layer), and emits a single consolidated signal to the Risk Management layer.

**Key files**

| File | Description |
|---|---|
| `strategy_base.hpp` | Abstract base class and lifecycle hook declarations |
| `strategy_manager.hpp / .cpp` | Multi-strategy orchestration |
| `strategy_context.hpp` | Dependency bundle injected into each strategy |
| `strategy_params.hpp` | Hot-reloadable atomic parameter struct |
| `momentum_scalper.hpp / .cpp` | EMA crossover momentum strategy |
| `orderbook_scalper.hpp / .cpp` | Order book imbalance strategy |
| `mean_reversion_scalper.hpp / .cpp` | Bollinger Band mean-reversion strategy |
| `signal_aggregator.hpp / .cpp` | Weighted voting signal combiner |

---

### Layer 4 — Heartbeat Scheduler

| Property | Value |
|---|---|
| Headers | `include/pulse/heartbeat/` |
| Sources | `src/heartbeat/` |

**Responsibility**

The Heartbeat Scheduler is the system's master clock for the AI analysis cycle. It fires every 5 minutes and drives the full chain from data collection through strategy parameter update, entirely asynchronously with respect to the market data thread.

- **HeartbeatScheduler** — Uses `asio::steady_timer` for high-precision 5-minute ticks. On each tick it enqueues an `OnBeat` task into the `TaskQueue`.
- **TaskQueue** — Thread-safe priority queue consumed by a dedicated worker thread. Supports cancellable tasks and prioritised execution. Prevents the WebSocket hot path from ever blocking on AI I/O.
- **Events**
  - `OnBeat` — Triggers the AI analysis pipeline.
  - `OnAnalysisDone` — Fired when `AIAnalyzer` completes; carries the `AnalysisResult` payload.
  - `OnParamUpdate` — Fired when `ParamAdvisor` has computed new parameter deltas and committed them to `StrategyParams`.

**Heartbeat flow**

```
HeartbeatScheduler (asio::steady_timer, 5 min)
        │
        ▼  enqueue OnBeat task
    TaskQueue  (priority queue, dedicated worker thread)
        │
        ▼
    AIAnalyzer  (TwitterFeed + NewsFeed + PromptBuilder + AIClient)
        │  AnalysisResult
        ▼
    ParamAdvisor  (computes parameter deltas)
        │  atomic writes to StrategyParams
        ▼
    StrategyManager  (strategies read new params on next tick, lock-free)
```

**Key files**

| File | Description |
|---|---|
| `heartbeat_scheduler.hpp / .cpp` | `asio::steady_timer`-based 5-minute clock |
| `task_queue.hpp / .cpp` | Thread-safe priority task queue |
| `heartbeat_events.hpp` | `OnBeat`, `OnAnalysisDone`, `OnParamUpdate` event types |

---

### Layer 5 — AI Analysis

| Property | Value |
|---|---|
| Headers | `include/pulse/ai/` |
| Sources | `src/ai/` |

**Responsibility**

The AI Analysis layer collects real-time social and news signals, assembles a structured prompt, calls a Large Language Model, and translates the response into actionable parameter deltas that the Strategy Engine can consume.

- **TwitterFeed** — Connects to the X (Twitter) API v2 filtered stream to gather tweets for tracked keywords (coin names, tickers, macro events). Maintains a rolling window of recent tweets for inclusion in prompts.
- **NewsFeed** — Polls NewsAPI and CryptoPanic for recent crypto headlines. Deduplicates and ranks articles by relevance score.
- **PromptBuilder** — Assembles the final prompt from current market snapshot, recent K-line data, tweet excerpts, news headlines, and current strategy parameters. Uses a fixed system prompt that enforces the JSON output schema.
- **AIClient** — Sends the assembled prompt to an LLM backend (OpenAI GPT-4o or Anthropic Claude) via HTTP (libcurl + OpenSSL). Handles authentication, request serialization, and response deserialization. Configurable backend and model via config file.
- **AnalysisResult** — C++ struct that is the exact mapping of the fixed JSON output schema:

```
{
  "sentiment":              "bullish" | "bearish" | "neutral",
  "direction_bias":         float,          // -1.0 to +1.0
  "volatility_forecast":    "low" | "medium" | "high",
  "confidence":             float,          // 0.0 to 1.0
  "recommended_param_deltas": {
    "entry_threshold_delta":  float,
    "position_size_delta":    float,
    "stop_loss_delta":        float,
    "take_profit_delta":      float
  }
}
```

- **ParamAdvisor** — Receives `AnalysisResult`, validates deltas against safety bounds, and applies them atomically to each active strategy's `StrategyParams`.

**Key files**

| File | Description |
|---|---|
| `twitter_feed.hpp / .cpp` | X API v2 filtered stream client |
| `news_feed.hpp / .cpp` | NewsAPI / CryptoPanic polling client |
| `prompt_builder.hpp / .cpp` | Context assembly and system prompt management |
| `ai_client.hpp / .cpp` | HTTP LLM backend client (OpenAI / Claude) |
| `analysis_result.hpp` | Fixed JSON schema C++ mapping |
| `param_advisor.hpp / .cpp` | Delta validation and atomic param application |

---

### Layer 6 — Risk Management

| Property | Value |
|---|---|
| Headers | `include/pulse/risk/` |
| Sources | `src/risk/` |

**Responsibility**

Every order signal from the Strategy Engine must pass through the Risk Management layer before reaching the Order Execution layer. This layer acts as a gate that enforces position limits, loss limits, and order rate limits.

- **RiskManager** — Central order gate. Evaluates each proposed order against all active risk rules. Returns an approval, a modification (reduced size), or a rejection with reason code.
- **PositionManager** — Tracks open positions across all strategies in real time. Aggregates net exposure per symbol and enforces portfolio-level limits (max notional, max number of open positions).
- **StopLossEngine** — Supports three stop modes per position: fixed (absolute price level), trailing (tracks best price with a percentage offset), and time-based (closes position after a maximum hold duration).
- **TakeProfitEngine** — Supports partial take-profit ladders: closes a configurable fraction of the position at each of N price targets, letting the remainder run until the stop.
- **DrawdownGuard** — Monitors rolling PnL (per strategy and aggregate). Automatically halts new order signals when daily drawdown or peak-to-valley drawdown exceeds configured thresholds.
- **OrderRateLimiter** — Implements a token-bucket algorithm to enforce Gate.io order submission rate limits and prevent API bans.

**Key files**

| File | Description |
|---|---|
| `risk_manager.hpp / .cpp` | Order gate and rule evaluation |
| `position_manager.hpp / .cpp` | Cross-strategy position tracking |
| `stop_loss_engine.hpp / .cpp` | Fixed / trailing / time-based stops |
| `take_profit_engine.hpp / .cpp` | Partial take-profit ladder |
| `drawdown_guard.hpp / .cpp` | Rolling drawdown circuit breaker |
| `order_rate_limiter.hpp / .cpp` | Token-bucket rate limiter |

---

### Layer 7 — Order Execution

| Property | Value |
|---|---|
| Headers | `include/pulse/execution/` |
| Sources | `src/execution/` |

**Responsibility**

The Order Execution layer is responsible for the reliable placement and lifecycle tracking of orders on Gate.io. It isolates the rest of the system from the complexity of network failures, partial fills, and exchange state inconsistencies.

- **OrderExecutor** — Submits orders via `GateRestClient`. Implements configurable retry logic (max attempts, back-off strategy) for transient failures. Supports market, limit, and post-only order types.
- **OrderTracker** — Monitors order state after submission. Primary path: listens to private order-update events on the WebSocket. Fallback path: polls REST order status endpoint when WS events are missed or the connection is interrupted.
- **ExecutionReport** — Immutable record generated for each completed order lifecycle. Contains: symbol, side, requested quantity, filled quantity, average fill price, slippage (vs. mid-price at submission), fees paid, and latency metrics.

**Key files**

| File | Description |
|---|---|
| `order_executor.hpp / .cpp` | Order submission with retry logic |
| `order_tracker.hpp / .cpp` | WS push + REST polling order state tracker |
| `execution_report.hpp` | Immutable fill record with slippage and fee fields |

---

### Layer 8 — Logging & Monitoring

| Property | Value |
|---|---|
| Headers | `include/pulse/logging/` |
| Sources | `src/logging/` |

**Responsibility**

The Logging & Monitoring layer provides observability for all system activity without imposing latency on the hot path. All I/O in this layer is asynchronous.

- **Logger** — Wraps `spdlog` with per-module named loggers (exchange, market, strategy, heartbeat, ai, risk, execution). Log level is configurable per module at runtime. Asynchronous sink with a bounded queue to prevent back-pressure on callers.
- **TradeRecorder** — Persists `ExecutionReport` records to CSV (default) or SQLite (optional, via SQLiteCpp). Used for post-session PnL analysis and audit trail.
- **MetricsCollector** — Accumulates and computes trading performance metrics on a rolling basis: net PnL, gross PnL, win rate, average win/loss ratio, Sharpe ratio, maximum drawdown, and trade count. Metrics are queryable at any time by the alert system or an external monitoring interface.
- **AlertManager** — Fires outbound alerts when configurable thresholds are breached (e.g., daily loss limit approached, strategy halted, WebSocket disconnect). Supports webhook (generic HTTP POST with JSON payload) and Telegram Bot API transports.

**Key files**

| File | Description |
|---|---|
| `logger.hpp / .cpp` | Per-module spdlog wrapper |
| `trade_recorder.hpp / .cpp` | CSV / SQLite execution record writer |
| `metrics_collector.hpp / .cpp` | Rolling PnL, Sharpe, drawdown metrics |
| `alert_manager.hpp / .cpp` | Webhook and Telegram alert dispatcher |

---

## 4. Data Flow

### Market data hot path (latency-critical)

```
Gate.io WS Gateway
      │  raw JSON frames
      ▼
GateWsClient  (Layer 1)
      │  typed channel callbacks
      ▼
MarketFeed dispatcher  (Layer 2)
      ├──► OrderBookManager  (incremental update, seqlock)
      ├──► KlineBuffer       (ring buffer, seqlock)
      └──► TickerCache       (atomic store)
              │
              ▼  lock-free reads
      StrategyEngine  (Layer 3)  — on_tick / on_orderbook / on_kline
              │
              ▼  trading signal
      RiskManager  (Layer 6)
              │  approved order
              ▼
      OrderExecutor  (Layer 7)
              │  ExecutionReport
              ▼
      TradeRecorder / MetricsCollector  (Layer 8)
```

### AI analysis cycle (background, every 5 minutes)

```
HeartbeatScheduler  (Layer 4)
      │  OnBeat task
      ▼
TaskQueue  (Layer 4)  — background worker thread
      │
      ▼
AIAnalyzer  (Layer 5)
      ├──► TwitterFeed   (X API v2 filtered stream)
      ├──► NewsFeed      (NewsAPI / CryptoPanic)
      ├──► PromptBuilder (market snapshot + signals)
      └──► AIClient      (OpenAI / Claude HTTP API)
              │  AnalysisResult (JSON)
              ▼
      ParamAdvisor  (Layer 5)
              │  atomic writes to StrategyParams
              ▼
      StrategyManager  (Layer 3)  — on_ai_update / param hot-reload
```

---

## 5. Threading Model

| Thread | Responsibility | Lifecycle |
|---|---|---|
| Main thread | Startup, config loading, component wiring | Blocks on shutdown signal |
| WebSocket I/O thread | `asio::io_context` for WS connection and heartbeat pings | Runs for process lifetime |
| Market data dispatch thread | Routes WS frames to Layer 2 sub-components | Runs for process lifetime |
| Strategy threads (N) | One `std::jthread` per active strategy; reads market data, emits signals | Started/stopped by StrategyManager |
| Heartbeat worker thread | Consumes `TaskQueue`; runs AI analysis cycle | Runs for process lifetime |
| Logger async thread | spdlog async queue consumer | Runs for process lifetime |

Strategies use `std::stop_token` (C++20) for cooperative cancellation. The heartbeat worker thread is the only thread that performs blocking network I/O to external APIs (AI, Twitter, News); it is explicitly isolated from all market data paths.

---

## 6. Key Design Decisions

### 1. C++20 over C++17

`std::jthread` paired with `std::stop_token` gives each strategy a clean, cooperative shutdown mechanism without manually managing `std::thread` join logic. `std::ranges` and range adaptors simplify K-line window operations (sliding windows, filtered projections) that would require verbose iterator arithmetic in C++17.

### 2. Heartbeat fully decoupled from the market data thread

AI inference calls (OpenAI, Claude) typically complete in 1–5 seconds and occasionally longer. Running them synchronously on the WebSocket I/O thread or the strategy dispatch thread would introduce multi-second stalls into the hot path, causing missed ticks and stale order book state. The `TaskQueue` ensures all AI I/O executes on a dedicated background thread with no shared locks on any market data structure.

### 3. Lock-free strategy parameter hot-update

`StrategyParams` stores all tunable numerical values as `std::atomic<double>`. This makes every read from a strategy thread a single relaxed atomic load — no mutex, no condition variable, no risk of priority inversion. `ParamAdvisor` uses `store(value, std::memory_order_release)` after each AI cycle; strategy threads use `load(std::memory_order_acquire)` on the next tick, guaranteeing visibility without a lock.

### 4. Fixed JSON schema for AI output

Open-ended LLM text responses are brittle to parse reliably. pulseTrader's system prompt instructs the LLM to return exactly one JSON object conforming to a documented schema (see `AnalysisResult` above). The `AIClient` validates the response against this schema using `nlohmann/json`; if validation fails, the result is discarded and the previous parameters remain in effect. This eliminates an entire class of runtime parsing failures.

### 5. Single exchange focus

Supporting multiple exchanges requires abstracting away the differences in WebSocket protocols, order types, rate limit semantics, and fee structures. This abstraction has a real cost: deeper per-exchange optimisations become harder to implement, and the code grows substantially more complex. pulseTrader intentionally targets Gate.io only, allowing it to exploit Gate.io-specific features (e.g., native order book channel with configurable depth levels, futures funding rate feed) without compromise.

---

## 7. Third-Party Dependencies

| Library | Version | Purpose |
|---|---|---|
| **libcurl** | ≥ 7.88 | HTTP client for REST API and AI backend calls |
| **OpenSSL** | ≥ 3.0 | HMAC-SHA512 signing, TLS for all outbound connections |
| **nlohmann/json** | ≥ 3.11 | JSON serialization / deserialization throughout |
| **spdlog** | ≥ 1.12 | Structured, asynchronous, per-module logging |
| **asio** (standalone) | ≥ 1.28 | Async I/O, `steady_timer` for heartbeat scheduler |
| **websocketpp** | ≥ 0.8.2 | WebSocket client (used by `GateWsClient`) |
| **GTest** | ≥ 1.14 | Unit and integration test framework |
| **fmt** | ≥ 10.0 | String formatting (used internally by spdlog and prompt builder) |
| **toml11** | ≥ 3.8 *(optional)* | TOML configuration file parsing |
| **SQLiteCpp** | ≥ 3.3 *(optional)* | SQLite persistence for `TradeRecorder` |

All dependencies are managed via **vcpkg** and declared in `vcpkg.json`. The optional dependencies (`toml11`, `SQLiteCpp`) are gated behind CMake feature flags (`-DPULSE_ENABLE_TOML=ON`, `-DPULSE_ENABLE_SQLITE=ON`).
