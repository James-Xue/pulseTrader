# pulseTrader — Implementation Roadmap

> **Version:** 0.1.0-dev\
> **Created:** 2026-06-15\
> **Strategy:** Vertical slice first — connect exchange → receive market data → place an order\
> Then expand horizontally with risk, strategy, AI, and WebUI.

---

## Principle

**先纵向打通最窄路径，再横向扩展。**

纵向路径 = Exchange → Market Data → Execution，能用 curl 对照调试、能拿真实行情、能下一笔测试单。这条链路跑通后，系统的"骨架"就成立了，后续每一层都是在骨架上加肌肉。

---

## Phase 1 — Foundation (Layer 2 + Layer 1)

> **Goal**: 编译通过 + 能连上 Gate.io + 能拿到账户信息和行情数据

### Step 1.1: Layer 2 — Logging

| Item | Detail |
|------|--------|
| Files | `logger.hpp / .cpp` |
| Scope | spdlog 异步 logger 封装，per-module named loggers |
| Key work | `PULSE_LOG_INFO/WARN/ERROR` 宏，async sink + bounded queue |
| Test | 单元测试：日志级别过滤、模块隔离、异步不阻塞调用方 |
| Why first | 所有后续层都需要日志，先有它等于磨刀 |

**Deliverable**: `tests/unit/test_logger.cpp` 通过，`cmake --build` 全量编译成功

### Step 1.2: Layer 1 — Exchange (REST)

| Item | Detail |
|------|--------|
| Files | `gate_auth.hpp / .cpp`, `gate_rest_client.hpp / .cpp` |
| Scope | HMAC-SHA512 签名、libcurl 封装、rate limit 处理、JSON 反序列化 |
| Key work | 先实现 `GET /api/v4/spot/currencies`（公开接口，验签无需 API key）→ 再实现 `GET /api/v4/spot/accounts`（私有接口，验签名） |
| Test | 集成测试：签名向量比对、mock HTTP server 测 retry/backoff |
| Depends on | Logger (L2) |

**Deliverable**: `tools/test_gate_rest.cpp` 成功拉取交易对列表和账户余额

### Step 1.3: Layer 1 — Exchange (WebSocket)

| Item | Detail |
|------|--------|
| Files | `gate_ws_client.hpp / .cpp`, `gate_ws_channels.hpp / .cpp` |
| Scope | websocketpp + asio 长连接、auto-reconnect (exponential backoff + jitter)、heartbeat ping、channel 订阅分发 |
| Key work | 先订阅 `spot.tickers` 公开频道验证连接 → 再订阅私有频道验证 WS 签名 |
| Test | 集成测试：连接→收到 ticker→断线重连→再次收到 ticker |
| Depends on | REST client (验签逻辑复用), Logger (L2) |

**Deliverable**: `tools/test_gate_ws.cpp` 订阅 ticker 频道，持续打印实时价格

---

## Phase 2 — Market Data Pipeline (Layer 3) ✅ COMPLETED

> **Goal**: 拿到结构化的实时行情数据，策略层可以直接消费
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

**Deliverable**: ✅ `tools/test_market_feed.cpp` 实时打印 BTC_USDT 的 ticker + 订单簿 top 5 + K线收盘价

---

## Phase 3 — Order Execution (Layer 8) ✅ COMPLETED

> **Goal**: 能手动触发下单，验证端到端链路
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

> ✅ **里程碑 M1**: 端到端链路打通。`Exchange → Market Data → Execution` 可运行，能拿到行情并下单。

---

## Phase 4 — Risk Management (Layer 7) ✅ COMPLETED

> **Goal**: 在下单前加一道安全闸门
> **Status**: ✅ Done (2026-06-16) — 92 unit tests, all 198 tests passing
> **Branch**: `feat/layer7-risk-management` (merged)

### Step 4.1: Foundation — risk_types + PositionManager ✅

| Item | Detail |
|------|--------|
| Files | `risk_types.hpp`, `position_manager.hpp / .cpp` |
| Scope | 共享类型（RiskDecision, RiskEvalResult, Position, PortfolioSummary）+ 线程安全持仓跟踪（shared_mutex），portfolio/symbol notional 限制 |
| Config | 新增 `StopMode` 枚举、`StopLossConfig`、`TakeProfitConfig`；`RiskConfig` 新增 `maxSymbolNotional` |
| Error codes | `RateLimitHit(3003)`, `StopLossTriggered(3004)`, `TakeProfitTriggered(3005)`, `SymbolLimitHit(3006)` |
| Test | 23 unit tests: open/close/limits/queries/aggregation/thread safety |

### Step 4.2: DrawdownGuard + OrderRateLimiter ✅

| Item | Detail |
|------|--------|
| Files | `drawdown_guard.hpp / .cpp`, `order_rate_limiter.hpp / .cpp` |
| Scope | 滚动 PnL 监控 + 日内/峰值回撤熔断器（atomic halt flag）；lock-free token-bucket 限流（atomic + CAS loop） |
| Test | 26 unit tests (14 + 12): equity tracking, drawdown triggers, token acquire/refill, thread safety |

### Step 4.3: RiskManager Orchestrator ✅

| Item | Detail |
|------|--------|
| Files | `risk_manager.hpp / .cpp` |
| Scope | 中央订单审批网关：`evaluate_order(OrderRequest)` → Approved / Modified(reduced qty) / Rejected(reason code) |
| Flow | DrawdownGuard halt check → OrderRateLimiter token check → PositionManager limit check |
| Test | 15 unit tests: approve/reject/modify across all rules, halt-clear recovery |

### Step 4.4: StopLossEngine + TakeProfitEngine ✅

| Item | Detail |
|------|--------|
| Files | `stop_loss_engine.hpp / .cpp`, `take_profit_engine.hpp / .cpp` |
| Scope | 三模式止损（Fixed/Trailing/TimeBased）+ 阶梯止盈（N targets + fractions），纯评估器不执行订单 |
| Test | 28 unit tests (16 + 12): fixed/trailing/time stops, ladder progression, multi-position tracking |

**Deliverable**: ✅ 92 unit tests 全覆盖，`pulse::risk` static library 编译通过

---

## Phase 5 — Strategy Engine (Layer 6)

> **Goal**: 自动产生交易信号，替代手动下单

### Step 5.1: Strategy Infrastructure

| Item | Detail |
|------|--------|
| Files | `strategy_base.hpp`, `strategy_context.hpp`, `strategy_params.hpp`, `strategy_manager.hpp / .cpp` |
| Scope | 抽象基类 + lifecycle hooks + hot-reload params + 多策略编排 |

### Step 5.2: MomentumScalper

| Item | Detail |
|------|--------|
| Files | `momentum_scalper.hpp / .cpp` |
| Scope | EMA crossover，第一个可运行的策略实现 |

### Step 5.3: OrderBookScalper + MeanReversionScalper

| Item | Detail |
|------|--------|
| Files | 两个策略的 `.hpp / .cpp` |
| Scope | 订单簿不平衡度策略 + 布林带均值回归策略 |

### Step 5.4: SignalAggregator

| Item | Detail |
|------|--------|
| Files | `signal_aggregator.hpp / .cpp` |
| Scope | 多策略加权投票 → 单一合并信号 |

**Deliverable**: `tools/test_strategy.cpp` 用历史行情回放，验证策略信号生成

> ✅ **里程碑 M2**: 自动交易。`Market Data → Strategy → Risk → Execution` 全自动闭环。

---

## Phase 6 — AI Pipeline (Layer 5 + Layer 4)

> **Goal**: 接入 LLM，实现参数自适应调整

### Step 6.1: AI Analysis (Layer 4)

| Item | Detail |
|------|--------|
| Files | `twitter_feed`, `news_feed`, `prompt_builder`, `ai_client`, `analysis_result`, `param_advisor` |
| Scope | 社交媒体/新闻采集 → prompt 组装 → LLM 调用 → JSON schema 验证 → 参数 delta 应用 |

### Step 6.2: Heartbeat Scheduler (Layer 5)

| Item | Detail |
|------|--------|
| Files | `heartbeat_scheduler`, `task_queue`, `heartbeat_events` |
| Scope | asio::steady_timer 5分钟节拍 → TaskQueue → AI 管线全链路 |

**Deliverable**: `tools/test_ai_pipeline.cpp` 模拟一次完整的心跳周期，验证参数更新

> ✅ **里程碑 M3**: AI 自适应。策略参数每 5 分钟根据市场情绪自动调整。

---

## Phase 7 — WebUI Dashboard (Layer 9)

> **Goal**: 浏览器实时监控，锦上添花

### Step 7.1: DashboardState + Snapshot Types

| Item | Detail |
|------|--------|
| Files | `dashboard_state.hpp / .cpp`, `snapshot_types.hpp` |
| Scope | 分层轮询线程、per-layer snapshot 数据结构 |

### Step 7.2: WebServer + WsServer

| Item | Detail |
|------|--------|
| Files | `web_server.hpp / .cpp`, `ws_server.hpp / .cpp` |
| Scope | uWebSockets HTTP(静态SPA) + WS(实时推送)、bearer token auth、Host header 校验 |

### Step 7.3: Frontend SPA

| Item | Detail |
|------|--------|
| Scope | 订单簿深度图、K线+信号标记、持仓/订单/PnL/AI分析卡片 |

**Deliverable**: `-DPULSE_ENABLE_WEBUI=ON` 编译后浏览器可访问全部面板

> ✅ **里程碑 M4**: 完整产品。全部 9 层可用，可对外推广。

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
