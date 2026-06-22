# pulseTrader 架构分析报告

> 分析日期: 2026-06-20
> 分析范围: 9 层架构、~70 个源文件
> 状态: 3/6 高优先级已修复，3 项待处理

## 已完成修复

| # | 问题 | 提交 | 说明 |
|---|---|---|---|
| 1 | PnL=0.0, 回撤保护失效 | `c857e21` | `close_position` 返回 `optional<double>`，main.cpp 累加后传给 DrawdownGuard |
| 2 | AI 参数调优断联 | `786e9f8` | `StrategyManager.all_params()` 收集真实 params，AiPipeline 循环写入所有策略 |
| 3 | `std::stod` 崩溃 WS 线程 | `7c052cf` | 新增 `safe_parse_double()`（from_chars），替换 34 处调用 + 10 个新测试 |

## 总览

| 类别 | 数量 | 代表性问题 |
|---|---|---|
| 🔴 正确性/安全 | 6 | PnL=0、AI断联、stod崩溃、TOCTOU竞态 |
| 🟡 性能/可维护性 | 12 | 连接不复用、全量快照拷贝、代码重复 |
| 🟢 代码质量 | 9 | 死代码、const_cast、废弃API |

**最值得优先处理的三件事：**
1. 接通 PnL 和 AI 反馈回路 — 交易系统的核心价值，目前是断的
2. 给所有 `std::stod` 加异常保护 — 一行修复，防崩溃
3. RiskManager 的 TOCTOU — 加一个 atomic "reserve" 操作防止仓位超限

---

## 🔴 高优先级 — 影响正确性 / 数据安全

### 1. PnL 永远是 0.0，回撤保护形同虚设

- **位置**: `apps/pulsetrader/main.cpp`，订单完成回调 lambda
- **现象**: `pnl` 硬编码为 `0.0`，`DrawdownGuard::record_pnl()` 从未收到真实盈亏
- **后果**: `maxDailyDrawdown` 限制永远不会触发，回撤保护功能不工作

### 2. AI 参数调优完全断开

- **位置**: `apps/pulsetrader/main.cpp` + `src/heartbeat/heartbeat_scheduler.cpp`
- **现象**: `HeartbeatScheduler` 写入的 `shared_params` 和各策略读取的 params 不是同一份内存
- **后果**: ParamAdvisor 算出的参数调整，策略根本收不到。整个 AI → 策略反馈回路断裂

### 3. `std::stod` 在 WS 事件线程上裸奔

- **位置**: `src/market/market_feed.cpp`、`src/execution/order_tracker.cpp` 多处
- **现象**: `std::stod()` 解析交易所返回的价格/数量字段，没有 try/catch
- **后果**: Gate.io 返回异常字符串（空串、"N/A"）时，`std::invalid_argument` 异常会干掉 WS 事件线程，导致行情中断或程序崩溃

### 4. RiskManager 存在 TOCTOU 竞态

- **位置**: `src/risk/risk_manager.cpp` — `evaluate_order()`
- **现象**: 依次调用 `can_open_position()` → `portfolio_summary()` → `symbol_notional()`，每次独立获取/释放 shared_lock
- **后果**: 两个策略线程可能在两次锁之间同时通过检查，各自下单，导致仓位超限
- **修复方向**: 需要原子的 "check-and-reserve" 操作

### 5. OrderTracker 完成回调在写锁下执行

- **位置**: `src/execution/order_tracker.cpp` — 订单状态变为终端态时
- **现象**: 回调里调用 `position_mgr.open_position()` 和 `drawdown_guard.record_pnl()`，锁持有期间还做了 spdlog I/O
- **后果**: 锁排序耦合风险 + 持锁 I/O 性能问题

### 6. ProxyTunnel 300 行代码塞在 `gate_ws_client.cpp` 里

- **位置**: `src/exchange/gate_ws_client.cpp` 匿名命名空间
- **现象**: 一个完整的 TCP 监听 + HTTP CONNECT 协商 + 双向中继的网络组件，没有独立头文件
- **后果**: 无法单独测试，生命周期管理复杂（detached thread 隐患，虽 M13 shutdown 修复做了 join）

---

## 🟡 中优先级 — 影响可维护性 / 性能

### 7. REST 请求不复用连接

- **位置**: `src/exchange/gate_rest_client.cpp` — `do_request()`
- **现象**: 每次 `curl_easy_init()` → 请求 → `curl_easy_cleanup()`，TCP+TLS 握手成本每次重付
- **影响**: 对当前调用频率可接受，但高频下单时会成为瓶颈

### 8. REST 重试时签名不刷新

- **位置**: `src/exchange/gate_rest_client.cpp` — `request()`
- **现象**: 签名（时间戳 + HMAC）在重试循环外计算一次，重试可能跨越数秒
- **影响**: Gate.io 时间窗口 ~60s，极端情况下签名会过期

### 9. WebUI DashboardState 每 200ms 全量快照拷贝

- **位置**: `src/webui/dashboard_state.cpp` — `poll_loop()`
- **现象**: 每 200ms 深拷贝整个 `DashboardSnapshot`（orderbook 40 档 + 100 根 K 线 + 仓位 + 订单 + 报告）
- **影响**: 堆分配密集，可用 COW 或 diff 推送优化

### 10. WebUI 只能看到一个市场

- **位置**: `apps/pulsetrader/main.cpp`
- **现象**: `auto& ui_feed = spot_feed ? *spot_feed : *futures_feed` — 优先 spot，否则 futures
- **影响**: 无法同时展示双市场数据

### 11. `completed_reports_` 无上限增长

- **位置**: `src/execution/order_tracker.cpp`
- **现象**: 已完成报告永远不清理，`recent_reports()` 的 O(n log n) 排序越来越慢
- **影响**: 长时间运行后内存持续增长

### 12. 三处辅助代码重复

- **位置**: `src/ai/news_feed.cpp`、`src/ai/twitter_feed.cpp`、`src/ai/ai_client.cpp`
- **重复内容**: `parse_iso8601`、`curl_write_callback`、`ensure_curl_init`
- **额外问题**: `parse_iso8601` 用了 `mktime()`，受本地时区/DST 影响，存在时区 bug

### 13. TwitterFeed URL 编码不完整

- **位置**: `src/ai/twitter_feed.cpp` — `url_encode()`
- **现象**: 只把空格编码为 `%20`，不处理 `#`、`&`、`=` 等特殊字符
- **影响**: 关键词含 `#bitcoin` 等时查询 URL 会损坏

### 14. SignalAggregator 在互斥锁下执行完整回调链

- **位置**: `src/strategy/signal_aggregator.cpp` (如果存在) 或 `strategy_manager.cpp` 中的 aggregator
- **现象**: `add_signal()` 持锁调用 `evaluate_and_emit()` → `output_callback_()`，后者触发风控评估 + 同步 REST 下单
- **影响**: 整个链路阻塞其他策略线程的信号提交

### 15. HeartbeatScheduler `io_context` 线程每 100ms 轮询

- **位置**: `src/heartbeat/heartbeat_scheduler.cpp`
- **现象**: `run_for(100ms)` 即使在 5 分钟间隔的空闲期也每 100ms 唤醒一次
- **修复方向**: 用 `io_ctx.run()` + `io_ctx.stop()` 按需阻塞/唤醒

### 16. 10 个策略参数非原子组更新

- **位置**: `src/ai/param_advisor.cpp` — `apply()`
- **现象**: 逐个写 `atomic<double>`，策略线程可能在更新到一半时读取
- **影响**: 同一评估周期可能用不一致的止损/止盈/仓位参数

### 17. Kline 的 `closed` 字段硬编码 `true`

- **位置**: `src/market/market_feed.cpp`
- **现象**: `kline.closed = true` — 无法区分已完成 K 线和正在形成的 K 线
- **影响**: 可能误导依赖 K 线完成状态的策略逻辑

### 18. MomentumScalper 没有本地冷却

- **位置**: `src/strategy/scalping/momentum_scalper.cpp` (如存在)
- **现象**: 其他两个策略有 `last_signal_time_ms_` 本地冷却，MomentumScalper 仅依赖 aggregator 全局冷却
- **影响**: 快速震荡行情中可能每个 K 线都发出信号

---

## 🟢 低优先级 — 代码质量 / 未来扩展

### 19. main.cpp 950 行手动接线

- **位置**: `apps/pulsetrader/main.cpp`
- **现象**: 组合根全靠手写，没有 DI 容器或 builder
- **影响**: 新增层或组件时容易遗漏初始化/关闭顺序

### 20. `const_cast` hack

- **位置**: `apps/pulsetrader/main.cpp`
- **现象**: `const_cast<pulse::webui::WsServer&>(ws_ref).push_snapshot(snap)` — `ws_server()` 返回 const 引用但 `push_snapshot()` 不是 const
- **修复方向**: `push_snapshot` 改为 const（内部 mutable）或 `ws_server()` 返回非 const

### 21. OrderBookManager 的 resubscribe 回调是死代码

- **位置**: `src/market/orderbook_manager.cpp` — `set_resubscribe_callback()`
- **现象**: 存在但从未被调用，序列号异常时系统静默使用可能过期的 orderbook

### 22. Heartbeat 事件类型已定义但从未使用

- **位置**: `src/heartbeat/heartbeat_events.hpp`
- **现象**: `OnBeat`、`OnAnalysisDone`、`OnParamUpdate`、`HeartbeatEvent` variant 已定义但从未构造/发射/消费
- **性质**: 未来事件总线的脚手架

### 23. `needs_json_ping()` 死代码

- **位置**: `src/exchange/endpoint_router.hpp`
- **现象**: 方法存在但从未被调用

### 24. WebUI 静态文件在事件循环线程上同步读磁盘

- **位置**: `src/webui/web_server.cpp` — `serve_static()`
- **现象**: `std::ifstream` 同步读文件在 uWS 事件循环线程上
- **影响**: 磁盘延迟会阻塞所有 HTTP/WS 处理

### 25. `sha512_hex` 使用 OpenSSL 已废弃 API

- **位置**: `src/exchange/gate_auth.cpp`
- **现象**: `SHA512()` 和 `HMAC()` 在 OpenSSL 3.x 上触发编译警告
- **修复方向**: 迁移到 EVP 接口

### 26. `curl_global_init` 散布在三个文件

- **位置**: `ai_client.cpp`、`news_feed.cpp`、`twitter_feed.cpp`
- **现象**: 各自用 `std::once_flag` 调用 `curl_global_init()`
- **修复方向**: 集中到一个初始化点（如 `core/curl_init.hpp`）

### 27. `curl_global_cleanup` 从未调用

- **现象**: 依赖 OS 回收
- **影响**: valgrind 等工具会报泄漏；如果 pulseTrader 被嵌入为库则有真实泄漏

---

## 架构全景图

```
┌─────────────────────────────────────────────────────────┐
│                    apps/pulsetrader/main.cpp              │
│                    (组合根, 手动接线)                      │
└──────┬──────┬──────┬──────┬──────┬──────┬──────┬────────┘
       │      │      │      │      │      │      │
   ┌───▼──┐┌──▼──┐┌──▼──┐┌─▼──┐┌──▼──┐┌──▼──┐┌──▼──┐
   │L1    ││L2   ││L3   ││L4  ││L5   ││L6   ││L9   │
   │Exch  ││Log  ││Mkt  ││AI  ││HB   ││Strat││WebUI│
   │      ││     ││     ││    ││     ││     ││     │
   │REST  ││spd  ││Feed ││Pipe││Sched││Mgr  ││uWS  │
   │WS ×2 ││log  ││OBook││Cli ││Task ││Aggr ││Dash │
   │Proxy ││     ││Ticker││News││Queue││×3   ││     │
   └──┬───┘└─────┘└──┬──┘└─┬──┘└──┬──┘└──┬──┘└──┬──┘
      │              │     │      │      │      │
      │         ┌────▼─────▼──────▼──────▼──────▼───┐
      │         │          L7 Risk                   │
      │         │  PosMgr | Drawdown | RateLimit     │
      │         │  StopLoss | TakeProfit             │
      │         └────────────┬───────────────────────┘
      │                      │
      │         ┌────────────▼───────────────────────┐
      │         │          L8 Execution               │
      │         │  OrderExec | OrderTracker | Report  │
      │         └────────────────────────────────────┘
      │
   ┌──▼──────────────┐    ┌─────────────────────┐
   │  Gate.io API     │    │ Optional             │
   │  REST + WS       │    │ L-SQLite TradeRec    │
   │  spot + futures  │    │                      │
   └─────────────────┘    └─────────────────────┘
```

## 线程清单

| 线程 | 归属 | 职责 |
|---|---|---|
| Main thread | `main()` | 200ms 轮询等待 SIGINT |
| Spot WS I/O | `GateWsClient` | 接收行情 + 订单事件 |
| Futures WS I/O | `GateWsClient` | 接收行情 + 订单事件 |
| Strategy thread(s) | `StrategyManager` | 每策略一个轮询线程 |
| DashboardState poll | `DashboardState` | 50ms 分级轮询 |
| uWS event loop | `WebServer` | HTTP + WS 处理 |
| io_context | `HeartbeatScheduler` | 定时器触发 |
| TaskQueue worker | `TaskQueue` | AI 管线执行 |

典型配置约 **8 个线程**。

## 数据流 (信号 → 下单 → 仓位)

```
Strategy thread
  → emit_signal(TradingSignal)
    → SignalAggregator.add_signal()  [mutex]
      → evaluate_and_emit()
        → output_callback_()         [仍持锁!]
          → RiskManager.evaluate_order()  [shared_lock ×3, TOCTOU]
            → OrderExecutor.place_order()  [sync REST POST]
              → OrderTracker.track_order()  [unique_lock, WS sub]
                → [async] on_order_update()  [WS I/O thread]
                  → completion_callback_()   [unique_lock]
                    → PositionManager.open_position()
                    → DrawdownGuard.record_pnl(0.0)  ← 问题所在
```

## 信号聚合机制

```
策略 A ──signal──┐
策略 B ──signal──┤→ SignalAggregator (per-symbol weighted accumulation)
策略 C ──signal──┘    │
                       ├─ normalize dominant confidence
                       ├─ threshold check
                       ├─ cooldown check (全局, 独立于策略本地冷却)
                       └─ emit consolidated signal → RiskManager
```

## 待讨论

- [x] #1 PnL 计算方案 — `close_position` 返回 `std::optional<double>`（已实现 PnL），main.cpp 累加后传给 `drawdown_guard.record_pnl()` ✅
- [x] #2 AI 反馈回路 — `StrategyManager.all_params()` 收集每个策略的真实 params 指针，`HeartbeatScheduler` + `AiPipeline::run()` 改为 `vector<StrategyParams*>`，ParamAdvisor 写入所有策略 ✅
- [x] #3 `std::stod` → `safe_parse_double()` — 34 处替换 + 10 个新测试，WS 事件线程不再因异常字符串崩溃 ✅
- [x] #4 TOCTOU: `PositionManager::reserve_notional()` 原子预留模式，单次 unique_lock 替代 3 次独立 shared_lock，`pending_reservations_` 防双花，`open_position()` 自动消耗预留 ✅
- [x] #5 回调从锁下移出："锁内收集，锁外执行"模式，`completion_callback_` 在 unique_lock 释放后调用，`set_completion_callback()` 加锁保护 ✅
- [x] #6 ProxyTunnel 提取为独立 `proxy_tunnel.hpp/.cpp`，修复 detached 线程 use-after-free + relay 注册竞态，删除 58 行死代码 ✅
