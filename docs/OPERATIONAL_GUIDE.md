# pulseTrader 操作指南：从零到实盘交易

> 本文档面向操作人员，说明如何将 pulseTrader 从当前状态推进到可实盘运行的交易系统。
>
> 最后更新：2026-06-19（M9 EndpointRouter + WS ping/pong 已完成，指南同步更新）

---

## 目录

1. [当前状态评估](#1-当前状态评估)
2. [缺失的关键模块](#2-缺失的关键模块)
3. [开发路线图](#3-开发路线图)
4. [操作流程（假设主程序就绪）](#4-操作流程假设主程序就绪)
5. [关键参数调优指南](#5-关键参数调优指南)
6. [风险控制体系](#6-风险控制体系)
7. [盈利性分析](#7-盈利性分析)
8. [风险警告](#8-风险警告)
9. [常见问题](#9-常见问题)

---

## 1. 当前状态评估

### 已完成的 9 层架构

| 层 | 模块 | 职责 | 状态 | 测试数 |
|----|------|------|------|--------|
| L1 | Exchange | Gate.io REST + WebSocket API | ✅ | 35 |
| L2 | Logging | spdlog 异步日志 | ✅ | 8 |
| L3 | Market Data | 行情热路径（延迟敏感） | ✅ | 33 |
| L4 | AI Analysis | 社交/新闻 → LLM → 参数调整 | ✅ | 43 |
| L5 | Heartbeat | 5 分钟 AI 时钟，TaskQueue | ✅ | 7 |
| L6 | Strategy | EMA 交叉 / 订单簿失衡 / 布林带均值回归 | ✅ | 52 |
| L7 | Risk Management | 仓位管理 / 回撤保护 / 限流 / 止损止盈 | ✅ | 92 |
| L8 | Execution | 订单生命周期管理 | ✅ | 22 |
| L9 | WebUI | uWebSockets 暗色 SPA 监控面板 | ✅ | 57 |

**467 测试全部通过** | 仅 `main` 分支 | Milestone M1–M9 全部达成

### 当前可运行的命令

```bash
./run.sh trade     # 启动交易主程序（9 层串联，完整交易系统）
./run.sh trade --config trading.toml  # 使用 TOML 配置文件启动
./run.sh rest      # 测试 Gate.io REST 连接（公开 + 私有接口）
./run.sh ws        # 测试 WebSocket 实时行情 + 私有频道
./run.sh market    # 测试行情数据管道（WS → L3 组件）
./run.sh strategy  # 测试策略引擎（模拟行情驱动 3 个策略）
./run.sh ai        # 测试 AI Pipeline（--mock 模式，不调用真实 LLM）
./run.sh webui     # 启动 WebUI 监控面板（浏览器 http://localhost:8080）
./run.sh test      # 运行全部 467 个单元测试
```

### 交易主程序（已完成）

`apps/pulsetrader/main.cpp`（约 630 行）串联 9 层，构成完整交易系统：

- **构造顺序**：L2 Logger → L1 Exchange → L3 Market → L7 Risk → L8 Execution → L6 Strategy → L4 AI → L5 Heartbeat → L9 WebUI
- **信号流**：StrategyManager → SignalAggregator → app callback（风控检查 → OrderExecutor → OrderTracker）
- **订单完成回调**：OrderTracker → PositionManager 开/平仓 + DrawdownGuard PnL 更新
- **优雅退出**：SIGINT/SIGTERM → 原子 stop flag → 反序停止（WebUI → Heartbeat → Strategy → Market → WS → Logger）
- **策略工厂**：`create_strategy()` 根据配置名称创建具体策略类（MomentumScalper / OrderBookScalper / MeanReversionScalper）
- **默认配置**：2 个策略运行于 BTC_USDT，AI 关闭，WebUI 监听 :8080，凭证从 `.env` 读取

所有现有命令均为 smoke test 工具，`./run.sh trade` 是唯一的生产级交易执行器。

---

## 2. 缺失的关键模块

### 必须实现（P0）

| 模块 | 说明 | 状态 |
|------|------|------|
| ~~交易记录器~~ | ~~SQLite 持久化每笔订单~~ | ✅ 已完成（Phase 2, M7） |
| **合约交易支持** | Gate.io USDT 永续合约（EndpointRouter + 双 WS + 合约 PnL/杠杆/保证金） | 🔲 Phase 5–7 (M10–M12) |
| — 配置基础 | MarketType/MarginMode 枚举, 合约配置字段, 7xxx 错误码 | ✅ 已完成（Phase 3, M8） |
| — 交换层路由 | EndpointRouter, WS ping/pong 泛化, 合约 REST 便捷方法 | ✅ 已完成（Phase 4, M9） |
| — 合约行情 | futures ticker/funding_rate/mark_price, SymbolInfo 合约乘数, 双 MarketFeed | 🔲 M10 |
| — 合约风控 | 统一 PnL 公式 (qty×price×multiplier), 杠杆/保证金检查, 强平价 | 🔲 M11 |
| — 合约执行 | futures 订单格式 (contract/signed size), OrderTracker 双市场, main.cpp 串联 | 🔲 M12 |

### 强烈建议（P1）

| 模块 | 说明 | 预估工时 |
|------|------|----------|
| **回测系统** | 用历史 K 线数据验证策略是否真正盈利（当前完全无回测能力） | 1–2 天 |
| **模拟交易模式** | Gate.io testnet 或本地模拟撮合，验证端到端流程 | 4–6h |
| **P&L 仪表盘** | WebUI 新增盈亏统计面板（日/周/月 P&L、胜率、盈亏比） | 3–4h |

### 锦上添花（P2）

| 模块 | 说明 | 预估工时 |
|------|------|----------|
| **Telegram/微信告警** | 关键事件推送（开仓/平仓/止损/回撤保护触发） | 2h |
| **多交易所支持** | 当前仅 Gate.io，扩展到其他交易所 | 1–2 周 |
| **策略热加载** | 运行时添加/移除策略，无需重启 | 4–6h |

---

## 3. 开发路线图

```
✅ Phase 0: 交易主程序                    ← 已完成（apps/pulsetrader/main.cpp, 9 层串联）
✅ Phase 1: TOML 配置文件加载             ← 已完成（config_loader + config_validator + trading.toml.example, 46 测试）
✅ Phase 2: SQLite 交易记录               ← 已完成（trade_recorder, 17 列表, 4 查询 API, 27 测试, M7 达成）
✅ Phase 3: 合约配置基础 (M8)             ← 已完成（MarketType/MarginMode 枚举, 合约字段, 7xxx 错误码, 18 测试）
✅ Phase 4: 合约交换层 (M9)                  ← 已完成（EndpointRouter + WS ping/pong 泛化, 合约 REST, 18 测试）
Phase 5: 合约行情数据 (M10)               ← futures ticker/funding_rate/mark_price, 双 MarketFeed, 预估 2-3 天
Phase 6: 合约风控 & PnL (M11)             ← 统一 PnL 公式, 杠杆/保证金检查, 预估 2-3 天
Phase 7: 合约执行 & 双市场串联 (M12)      ← futures 订单格式, main.cpp 双 WS/Feed, 预估 3-4 天
Phase 8: Gate.io testnet 模拟交易 1 周     ← 预估 1 周
Phase 9: P&L 分析 + 策略调优               ← 预估 2–3 天
Phase 10: 小资金实盘（100 USDT）           ← 持续观察
Phase 11: 逐步加仓                         ← 根据数据决策
```

### Phase 2 详细任务（已完成）

```
src/trade_recorder/（新增，已完成）:
  ├── trade_record.hpp — TradeRecord (17 字段) + TradeSummary POD structs
  ├── trade_recorder.hpp/cpp — RAII TradeRecorder, SQLite::Database, WAL + mutex
  ├── 建表：trades (17 列: id, order_id, client_order_id, timestamp_ns, symbol,
  │   side, order_type, requested_qty, filled_qty, avg_fill_price, submit_mid_price,
  │   slippage_bps, fees, pnl, latency_ms, final_status, strategy_name)
  ├── 4 个查询 API: get_trades / get_trades_by_strategy / get_summary / get_daily_pnl
  ├── record_trade() — 线程安全 INSERT (mutex-guarded, UNIQUE order_id)
  └── CMake: -DPULSE_ENABLE_SQLITE=ON 启用

apps/pulsetrader/main.cpp（已改造）:
  ├── #ifdef PULSE_ENABLE_SQLITE 初始化 TradeRecorder
  ├── OrderTracker 完成回调中调用 recorder.record_trade()
  ├── sig.strategy_id 通过 client_order_id 透传到 trade_recorder
  └── 优雅退出时 checkpoint + close

测试（27 个，全部通过）:
  ├── test_trade_recorder.cpp — 15 核心测试
  └── test_trade_queries.cpp — 12 查询测试
```

### Phase 8 详细任务

```
Phase 8: Gate.io testnet 模拟交易（M12 完成后）:
  ├── 修改 config.hpp: restBaseUrl / wsUrl 切换到 testnet 地址
  ├── Gate.io testnet 仅支持合约，不支持现货 — 需要评估替代方案
  ├── 可能的替代：本地模拟撮合引擎（paper trading mode）
  └── 运行 1 周收集数据，验证策略盈利能力
```

---

## 4. 操作流程（假设主程序就绪）

### 4.1 环境准备

```bash
# 1. Gate.io 创建子账户
#    - 目的：隔离风险，一个子账户跑一个策略组合
#    - 最多 10 个子账户（VIP 0–4）或 30 个（VIP 5–9）
#    - 子账户继承主账户 VIP 等级
#    - ⚠️ 子账户创建后不可删除

# 2. 子账户充值启动资金
#    - 建议先用 100–500 USDT 试水
#    - 确认子账户有足够 USDT 用于交易

# 3. 创建 API Key
#    - ⚠️ 只开"现货交易"权限，不开"提币"权限！
#    - ⚠️ IP 白名单：填入服务器公网 IP
#    - 记录 API Key 和 Secret

# 4. 配置 .env 文件
cat > .env << 'EOF'
GATE_API_KEY=your_sub_account_api_key
GATE_API_SECRET=your_sub_account_api_secret
HTTPS_PROXY=http://127.0.0.1:7897
HTTP_PROXY=http://127.0.0.1:7897
EOF

# 5. 确认 .env 已被 gitignore（已有）
grep ".env" .gitignore  # 应该有输出
```

### 4.2 编写配置文件

```toml
# trading.toml — pulseTrader 交易配置
# 完整模板见 trading.toml.example

# 顶级键必须在所有 [section] 之前
symbols = ["BTC_USDT", "ETH_USDT"]

[exchange]
apiKey = "from_env:GATE_API_KEY"
apiSecret = "from_env:GATE_API_SECRET"
restBaseUrl = "https://api.gateio.ws"
wsUrl = "wss://api.gateio.ws/ws/v4/"
proxyUrl = "from_env:HTTPS_PROXY"
restTimeoutMs = 10000
maxRetries = 3

[log]
level = "info"
logDir = "logs"
toConsole = true
toFile = true

# --- 策略配置 ---
[strategy]
signal_aggregator_threshold = 0.7   # 聚合信号置信度 ≥ 0.7 才执行
signal_cooldown_sec = 30             # 同一币种信号冷却 30 秒

[[strategy.instances]]
name = "momentum_scalper"
symbol = "BTC_USDT"
order_quantity = 0.001               # 每笔 0.001 BTC（约 $65）
min_confidence = 0.6
poll_interval_ms = 200               # 200ms 轮询一次行情

[[strategy.instances]]
name = "orderbook_scalper"
symbol = "BTC_USDT"
order_quantity = 0.001
min_confidence = 0.65
poll_interval_ms = 100               # 订单簿策略需要更频繁

[[strategy.instances]]
name = "mean_reversion_scalper"
symbol = "ETH_USDT"
order_quantity = 0.01                # 每笔 0.01 ETH（约 $35）
min_confidence = 0.6
poll_interval_ms = 500

# --- 风控配置 ---
[risk]
maxPositionNotional = 500            # 最大持仓 500 USDT
maxOpenPositions = 3                 # 最多同时 3 个仓位
maxDailyDrawdown = 0.02              # 日亏 ≥ 2% 停机
maxDrawdown = 0.05                   # 总回撤 ≥ 5% 全部停止
maxOrdersPerSec = 5                  # 每秒最多 5 笔订单
maxSymbolNotional = 300              # 单币种最大持仓 300 USDT

[risk.stop_loss]
mode = "Trailing"                    # 追踪止损
trailing_pct = 0.005                 # 0.5% 追踪偏移
max_hold_seconds = 300               # 最长持仓 5 分钟

[risk.take_profit]
enabled = true
targets_pct = [0.005, 0.01, 0.02]   # 0.5% / 1% / 2% 三档止盈
fractions = [0.33, 0.33, 0.34]       # 每档平仓 33% / 33% / 34%

# --- AI 配置 ---
[ai]
backend = "openai"                   # 或 "claude"
model = "gpt-4o"
apiKey = "from_env:OPENAI_API_KEY"
heartbeatIntervalSec = 300           # 每 5 分钟 AI 分析一次
requestTimeoutMs = 30000

# --- WebUI 配置 ---
[webui]
enabled = true
bindAddress = "127.0.0.1"
port = 8080
authToken = "your-secret-token-here"
maxClients = 4
```

### 4.3 启动交易

```bash
# 终端 1：启动交易主程序
./run.sh trade --config trading.toml

# 预期输出：
# [INFO] pulseTrader v0.1.0 starting...
# [INFO] Exchange: Gate.io (REST + WS connected)
# [INFO] Market Data: subscribed to BTC_USDT, ETH_USDT
# [INFO] Strategies: 3 instances started
#   - momentum_scalper on BTC_USDT (200ms poll)
#   - orderbook_scalper on BTC_USDT (100ms poll)
#   - mean_reversion_scalper on ETH_USDT (500ms poll)
# [INFO] Risk Manager: max notional 500 USDT, daily DD limit 2%
# [INFO] AI Pipeline: heartbeat every 300s, next run in 5min
# [INFO] WebUI: http://127.0.0.1:8080
# [INFO] Trading engine started. Press Ctrl+C to stop.
```

### 4.4 监控运行

```bash
# 终端 2：打开 WebUI
./run.sh webui
# 浏览器访问 http://localhost:8080

# 或直接查看日志
tail -f logs/strategy.log    # 策略信号
tail -f logs/execution.log   # 订单执行
tail -f logs/risk.log        # 风控事件
tail -f logs/ai.log          # AI 分析结果
```

### 4.5 停止交易

```bash
# 优雅退出：Ctrl+C 或发送 SIGTERM
# 主程序会：
#   1. 停止策略引擎（不再产生新信号）
#   2. 平掉所有持仓（market order 平仓）
#   3. 关闭 WS 连接
#   4. 刷新日志
#   5. 退出
```

---

## 5. 关键参数调优指南

### 5.1 策略参数

| 参数 | 含义 | 调优方向 |
|------|------|----------|
| `order_quantity` | 每笔下单量 | 从小开始（0.001 BTC），验证盈利后逐步加大 |
| `min_confidence` | 信号置信度门槛 | 越高越保守（少交易但精准），越低越激进（多交易但噪音多） |
| `poll_interval_ms` | 行情轮询频率 | 越低延迟越好，但 CPU 占用更高。建议 100–500ms |
| `signal_aggregator_threshold` | 聚合信号执行门槛 | 0.7 = 需要多个策略共识才下单，降低则单策略也能触发 |
| `signal_cooldown_sec` | 同币种信号冷却 | 防止连续下单。剥头皮建议 15–60 秒 |

### 5.2 EMA 交叉策略 (momentum_scalper)

```cpp
// strategy_params.hpp 中的可调参数
ema_fast_period     = 9       // 快线周期（越小越灵敏）
ema_slow_period     = 21      // 慢线周期（越大越平滑）
ema_crossover_thresh = 0.001  // 交叉阈值（0.1%）
```

**调优建议**：
- 震荡行情：加大 `ema_slow_period`（如 50），减少假信号
- 趋势行情：减小 `ema_fast_period`（如 5），更快捕捉趋势

### 5.3 订单簿失衡策略 (orderbook_scalper)

```cpp
ob_imbalance_window  = 5       // 深度层数
ob_imbalance_thresh  = 0.6     // 买卖比阈值（0.6 = 买方量占 60%）
ob_refresh_ms        = 100     // 订单簿刷新间隔
```

**调优建议**：
- 高波动市场：降低阈值到 0.55，更容易触发信号
- 低流动性币种：减少深度层数到 3，只看近盘

### 5.4 布林带均值回归策略 (mean_reversion_scalper)

```cpp
bb_period            = 20      // 布林带周期
bb_std_dev           = 2.0     // 标准差倍数
bb_entry_thresh      = 0.001   // 触碰带边后入场阈值
```

**调优建议**：
- 适合震荡行情（BTC 横盘时）
- 趋势行情中应禁用此策略（会逆势开仓）

### 5.5 AI 调参

AI 每 5 分钟分析一次社交/新闻情绪，输出 `ParamDeltas` 调整策略参数：

```json
{
  "ema_fast_delta": -1,       // 加快 EMA 快线
  "ema_slow_delta": 0,
  "ob_thresh_delta": 0.05,    // 提高订单簿阈值
  "bb_std_delta": -0.2,       // 收窄布林带
  "confidence_delta": 0.05,   // 提高置信度门槛
  ...
}
```

**注意**：AI 调参效果高度依赖 prompt 设计。初期建议**关闭 AI 调参**，先验证基础策略的盈利能力。

---

## 6. 风险控制体系

### 6.1 多层风控

```
信号产生 → 信号聚合 → 风控检查 → 下单 → 持仓监控 → 止损/止盈
                                    ↓
                              以下任一条件不满足则拒绝：
                              - 总持仓 < maxPositionNotional
                              - 单币种 < maxSymbolNotional
                              - 仓位数 < maxOpenPositions
                              - 下单频率 < maxOrdersPerSec
                              - 日亏损 < maxDailyDrawdown
                              - 总回撤 < maxDrawdown
```

### 6.2 止损策略

| 模式 | 说明 | 适用场景 |
|------|------|----------|
| **Fixed** | 入场价 ±1% 固定止损 | 简单明了，适合新手 |
| **Trailing** | 追踪最优价，回撤 0.5% 触发 | **推荐**，适合趋势行情 |
| **TimeBased** | 持仓超过 5 分钟强制平仓 | 超短线剥头皮 |

### 6.3 止盈阶梯

```
入场价 → +0.5% 平 33% → +1.0% 平 33% → +2.0% 平 34%
```

分批止盈可以：
- 锁定部分利润，避免利润回吐
- 让剩余仓位享受更大涨幅
- 降低单次决策的风险

### 6.4 熔断机制

| 条件 | 动作 |
|------|------|
| 日亏损 ≥ 2% | 停止开新仓，已有仓位继续管理 |
| 总回撤 ≥ 5% | 全部平仓，系统停机，需手动重启 |
| 下单频率超限 | 丢弃多余信号，记录告警 |

### 6.5 操作安全

- ✅ API Key 只开交易权限，**不开提币权限**
- ✅ 使用子账户隔离风险
- ✅ IP 白名单限制 API 访问
- ✅ `.env` 文件已 gitignore
- ⚠️ 当前配置使用**主网**（非 testnet），真金白银

---

## 7. 盈利性分析

### 7.1 手续费是最大敌人

| 项目 | 费率 | 说明 |
|------|------|------|
| Gate.io 现货 Taker | 0.2% | 吃单方（market order） |
| Gate.io 现货 Maker | 0.2% | 挂单方（limit order） |
| 一个来回 | **0.4%** | 买入 + 卖出 |
| VIP 1（≥100万/月） | 0.15% | 一个来回 0.3% |
| 点卡支付 | 8折 | 用 GT 代币支付手续费 |

### 7.2 盈亏平衡计算

```
假设：
  - 每笔交易 0.001 BTC ≈ $65
  - 手续费一个来回 0.4% = $0.26
  - 滑点估算 0.1% = $0.065

盈亏平衡：
  - 每笔利润必须 > $0.325（0.5%）才能覆盖成本
  - 如果胜率 55%，盈亏比需要 > 0.82:1
  - 如果胜率 50%，盈亏比需要 > 1.0:1（即平均盈利 = 平均亏损）

结论：
  - 剥头皮利润空间极窄（0.5%–2%）
  - 手续费 + 滑点吃掉 20%–60% 的利润
  - 需要胜率 > 55% 或盈亏比 > 1.5:1 才能稳定盈利
```

### 7.3 延迟的影响

```
你的设置（国内 + 代理）：
  - 网络延迟：~100–200ms（到 Gate.io 服务器）
  - 代理额外延迟：~20–50ms
  - 总往返延迟：~250–500ms

真正的 HFT：
  - Co-location：~1ms
  - 同机房：~0.1ms

结论：
  - 你不可能是最快的，不要和机构拼速度
  - 适合做 1–5 分钟级别的中频策略
  - 避免做秒级剥头皮（会被更快的对手方吃掉）
```

### 7.4 合理的盈利预期

| 场景 | 月收益率 | 条件 |
|------|----------|------|
| 保守 | 2–5% | 低频率、严格风控、震荡行情 |
| 中性 | 5–10% | 中频率、策略有效、市场配合 |
| 激进 | 10–20% | 高频率、大仓位、承担高风险 |
| 亏损 | -5% ~ -100% | 策略无效、黑天鹅、风控失效 |

**现实**：大部分个人量化交易者最终是亏损的。机构有速度优势、数据优势、资金优势。个人量化的核心竞争力在于：灵活性（可以快速切换策略）和零管理费。

---

## 8. 风险警告

### 8.1 技术风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 网络断连 | 无法平仓 | 止损单 + 手动平仓备用通道 |
| 代理故障 | 行情延迟 | 健康检查 + 自动重连 |
| 程序 Bug | 错误下单 | 风控层拦截 + 小资金试跑 |
| API 变更 | 接口失效 | 版本锁定 + 错误处理 |
| 服务器宕机 | 无人值守 | 云服务商 + 监控告警 |

### 8.2 市场风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 闪崩 | 瞬间巨亏 | 日亏损熔断（2%） |
| 流动性枯竭 | 滑点巨大 | 限仓（单币种 300 USDT） |
| 交易所故障 | 无法交易 | 分散到多交易所 |
| 策略失效 | 连续亏损 | 回撤熔断（5%） |

### 8.3 操作风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| API Key 泄露 | 资产被盗 | 不开提币权限 + IP 白名单 |
| 误操作 | 意外下单 | testnet 先行 + 确认流程 |
| 配置错误 | 参数异常 | 配置文件校验 + 合理默认值 |

---

## 9. 常见问题

### Q1: 为什么不用 testnet？

Gate.io 有 testnet（`https://fx-api-testnet.gateio.ws`），但：
- 仅支持合约（futures），不支持现货
- 现货交易只能用主网
- 建议：先用小资金（100 USDT）在主网跑，当作"学费"

### Q2: 可以同时跑多少个策略？

当前架构支持多策略并行（每个策略一个 `std::jthread`），实际受限于：
- CPU 核心数（每个策略一个线程 + 行情线程）
- 风控限制（`maxOpenPositions = 5`）
- 建议：初期 2–3 个策略，观察效果后再增加

### Q3: AI 调参真的有用吗？

**不确定**。这是一个实验性功能：
- LLM 分析社交/新闻情绪 → 输出参数调整建议
- 有效性完全取决于 prompt 设计和市场状态
- 建议初期**关闭 AI 调参**（`heartbeatIntervalSec = 0`），先验证基础策略
- 确认基础策略盈利后，再开启 AI 观察效果

### Q4: 为什么选择 Gate.io？

- REST + WebSocket API 文档完善
- 支持子账户（风险隔离）
- 手续费相对合理（0.2%）
- 流动性可接受（BTC/ETH 主流币对）
- 缺点：延迟不如 Binance，国内需要代理

### Q5: 如何判断策略是否有效？

运行 1 周后统计：

| 指标 | 合格线 | 优秀线 |
|------|--------|--------|
| 胜率 | > 50% | > 60% |
| 盈亏比 | > 1.0 | > 1.5 |
| 夏普比率 | > 1.0 | > 2.0 |
| 最大回撤 | < 10% | < 5% |
| 日交易次数 | 5–20 | 10–30 |
| 净利润（扣费后） | > 0 | 月化 > 5% |

### Q6: 遇到问题怎么排查？

```bash
# 1. 检查连接
./run.sh rest    # REST 是否通？
./run.sh ws      # WS 是否收到实时行情？

# 2. 检查行情
./run.sh market  # L3 组件是否正常更新？

# 3. 检查策略
./run.sh strategy  # 策略是否产生信号？

# 4. 检查 AI
./run.sh ai      # AI 是否正常返回分析结果？

# 5. 查看日志
ls logs/
cat logs/exchange.log   # 连接错误？
cat logs/strategy.log   # 信号异常？
cat logs/risk.log       # 风控触发？
cat logs/execution.log  # 下单失败？

# 6. WebUI
./run.sh webui
# 浏览器打开 http://localhost:8080 查看实时状态
```

---

## 附录：快速参考卡

```
┌─────────────────────────────────────────────────┐
│              pulseTrader 操作速查                 │
├─────────────────────────────────────────────────┤
│  启动:  ./run.sh trade --config trading.toml     │
│  监控:  ./run.sh webui → http://localhost:8080   │
│  停止:  Ctrl+C（优雅退出，自动平仓）               │
│  测试:  ./run.sh test（467 个单元测试）            │
│  日志:  tail -f logs/*.log                       │
├─────────────────────────────────────────────────┤
│  .env:        API Key / Secret / Proxy           │
│  trading.toml: 策略参数 / 风控 / AI              │
│  子账户:      隔离风险，不开提币权限               │
│  熔断:        日亏 2% 停 / 总回撤 5% 全停         │
├─────────────────────────────────────────────────┤
│  ⚠️  主网 = 真金白银                              │
│  ⚠️  手续费一个来回 0.4%                          │
│  ⚠️  延迟 ~250-500ms，不要和机构拼速度            │
│  ⚠️  先小资金试跑，验证盈利后再加仓                │
└─────────────────────────────────────────────────┘
```
