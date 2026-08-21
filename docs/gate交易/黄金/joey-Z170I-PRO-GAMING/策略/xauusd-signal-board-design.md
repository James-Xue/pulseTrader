# XAUUSD 子代理 × 引擎信号板(SignalBoard)设计与实施指南

> **版本 1.1 · 2026-08-17 · 状态:已实施(731 测试绿,引擎 signal_only 上线)**

## 实施修订记录

- **§3.7 place_trigger_order 改为"open_order 附加 SL/TP"**:实施中发现 08-16 备忘的 price_orders 是**期货专属**(备忘原文标注"现货/CFD 不适用")。CFD 的正确机制是 MT5 订单体自带的 `price_sl`/`price_tp`(docs/CFD_TRADFI.md 已记录,OrderExecutor buildOrderBody 原注释预留)→ 落地为:`OrderRequest.sl_price/tp_price`(optional)+ buildOrderBody CFD 分支填充 + `open_order` 参数 `sl_price`/`tp_price`(REPL `--sl/--tp`、MCP schema),非 cfd 明确拒绝。效果等同原设计目标(交易所原生止损,引擎/子代理全挂也生效),且少一个新方法。独立挂触发单保护已有持仓(非入场单)的场景暂不支持,需要时另测 tradfi trigger 语义。
- **闸门位置**:`signal_only` 放进 `OrderFlowExecutor::onSignal` 入口(Flat 检查后、方向门前),而非 main 回调——OrderFlowTest 可直接测试,手动 placeOrder 天然不受影响。
- **指标快照**:TradingSignal 增加 `indicators`(json,默认 `{}`),四个策略已填(见 §3.3 表格)。
- **基线偏移**:实施基于 commit `21f76ab`(M19.1),main.cpp 行号与 1.0 版略有平移(策略回调 919、聚合回调 1058、EngineServices 1072),结构不变。

> **版本 1.0 · 2026-08-17**
> **基线**:pulseTrader `headless` 分支,commit `1ced101`(本机另有会话在改码,实施前请先 `git pull` 对齐;本文行号基于上述基线,若代码已前进请以实际为准)
> **读者**:负责实施的 Claude Code 会话。本文是"改什么、怎么改"的规格书,不含可整段粘贴的代码;所有行号/文件名指向基线代码,照模式改即可。
> **配套文件**:`xauusd-subagent-strategy.md`(子代理规则基线 v1,§4 为其 v2,落地后回填)

---

## 1. 背景与目标

**冲突**:引擎 3 个 XAUUSD CFD 策略(momentum/mean_reversion/supertrend)按配置自动下单,与 LLM 子代理的下单决策互相不知情 → 双头交易、互平仓位、突破"最多 1 仓"约束。

**方案**:信号生成与下单执行**解耦**。策略照常运行,但只发布信号到因子板(SignalBoard);子代理定期读取(`get_signals`)作为决策因子,是**唯一下单者**(走手动路径 `open_order`/`close_position`,风控全保留)。用户持续添加"策略工具",每个工具 = 一个新因子,全部汇入因子板。

**目标**:

- **G1** 引擎可配置为 signal-only:策略信号不触发下单
- **G2** 每个策略的实时信号 + 指标快照发布到因子板
- **G3** `get_signals` 经 JSON-RPC / MCP / REPL 三通道可读
- **G4** 聚合器共识信号作为独立因子一并发布(与 signal_only 无关,两种模式都发,复盘可审计)
- **G5** 手动下单路径**不受** signal_only 影响(这是子代理的通道)
- **G6** 扩展点:后续新增因子工具时,子代理侧接口零变更

---

## 2. 架构总览

```
策略线程 × N(每 500ms 轮询)
   │  TradingSignal(新增 indicators 字段)
   ├──────────────────────────► SignalAggregator ──聚合信号──► (main.cpp 输出回调)
   │                                    │                          │
   │ 原始信号 + 指标快照                  │                          ▼
   └──► SignalBoard(因子板) ◄── publishAggregate ──────────  OrderFlowExecutor.onSignal
            │  最新 per strategy_id + 聚合槽位                    │ signal_only=true → 直接 return
            │                                                    │ signal_only=false → 风控→下单(现状)
            ▼  get_signals = EngineServices::signals()
   控制平面(JSON-RPC 8081 / MCP stdio / REPL)
            ▲
            │  每轮读取(新鲜度 ≤120s 的因子才有效)
        子代理(LLM)── 自研规则(§4)+ 因子 → open_order/close_position(手动路径,绕过聚合器)
```

要点:

- 手动路径(`openOrder`)与信号路径(`onSignal`)在 `OrderFlowExecutor` 中是**不同入口**,所以 signal_only 闸门只作用于信号路径,天然不影响子代理下单(实施时核实函数名,见 §7)
- 因子板是**内存结构**,引擎重启即清空(子代理按新鲜度过滤即可,无需持久化)

---

## 3. 引擎侧实施规格

### 3.1 配置:`signal_only`

- **位置**:`StrategyConfig`(`src/core/config.hpp:288`,现含 `signal_aggregator_threshold` / `signal_cooldown_sec`)
- **字段**:`bool signal_only = false;` · TOML:`[strategy]` 节下 `signal_only = false`
- **loader**:与其他字段同样模式读取(find_or);**validator**:无约束(布尔)
- **语义**:`true` = 策略信号只进因子板,聚合输出不下单;手动 `open_order`/`close_position` 不受影响

### 3.2 新组件 SignalBoard

- **文件**:`src/strategy/signal/SignalBoard.hpp` + `.cpp`(与 `SignalAggregator` 同目录);加入 `src/strategy/CMakeLists.txt`
- **数据结构**:

```cpp
namespace pulse::strategy
{
struct FactorEntry
{
    std::string source;      // = strategy_id,如 "momentum_scalper_XAUUSD";聚合槽位为 "aggregate"
    Symbol symbol;           // "XAUUSD"
    MarketType market_type;  // Cfd / Futures / Spot
    SignalType type;         // Buy / Sell / Flat
    double confidence;       // 0..1
    Price price;             // 信号参考价
    std::int64_t ts_ms;      // 引擎时钟毫秒
    std::string reason;      // 人类可读原因
    nlohmann::json indicators; // 策略指标快照(§3.3),可为 {}
};
}
```

- **类接口**:

```cpp
class SignalBoard
{
  public:
    explicit SignalBoard(double aggregateThreshold);
    void publish(const TradingSignal &sig);          // 按 strategy_id 覆盖式保存最新一条
    void publishAggregate(const TradingSignal &sig); // 聚合槽位(带 threshold)
    [[nodiscard]] nlohmann::json snapshot() const;               // 全量
    [[nodiscard]] nlohmann::json snapshot(MarketType mt) const;  // 按市场过滤
    [[nodiscard]] std::uint64_t entryCount() const;

  private:
    mutable std::shared_mutex m_mutex;
    std::map<std::string, FactorEntry> m_latest;  // key = strategy_id
    std::optional<FactorEntry> m_aggregate;
    double m_aggregateThreshold;
};
```

- **线程安全**:`publish` 来自多个策略线程(500ms 一次,低频),`snapshot` 来自控制线程 → `shared_mutex` 足够,热路径开销可忽略
- **`snapshot()` 返回 JSON 结构**(`ts_str` 遵循 `[control] display_timezone`,复用 `src/core/TimeUtil.hpp` 的既有格式化,与其它方法一致):

```json
{
  "signals": [
    {
      "source": "momentum_scalper_XAUUSD",
      "symbol": "XAUUSD", "market_type": "cfd",
      "type": "buy", "confidence": 0.72, "price": 4401.2,
      "ts_ms": 1786943000000, "ts_str": "2026-08-17T13:03:20+00:00",
      "reason": "EMA9 crossed above EMA21",
      "indicators": { "ema_fast": 4400.1, "ema_slow": 4399.7 }
    }
  ],
  "aggregate": {
    "source": "aggregate", "symbol": "XAUUSD", "market_type": "cfd",
    "type": "flat", "confidence": 0.42, "threshold": 0.6,
    "ts_ms": 1786943000000, "ts_str": "...", "reason": "...", "indicators": {}
  },
  "updated_at_ms": 1786943000000
}
```

### 3.3 策略指标快照

- **改 `TradingSignal`**(`src/strategy/signal_types.hpp:49`):新增字段 `nlohmann::json indicators;`,默认 `{}`(构造函数补默认初始化)。聚合器与既有测试不受影响(空 JSON),加一条默认构造断言测试
  - 备选方案(不推荐,除非实施会话有强理由):不动 TradingSignal,另给 StrategyManager 加 `getLastIndicators(id)` 接口。改动面更大
- **每个策略在 `emitSignal` 前把已计算的指标塞进 `indicators`**。v1 原则:**只发现成的值,不新增计算**(新增计算留 Phase 4 的因子工具):

| 策略 | indicators 建议字段(以策略内实际变量名为准) |
|---|---|
| MomentumScalper | `ema_fast`, `ema_slow`, `ema_diff`, `last_close` |
| MeanReversionScalper | `bb_mid`, `bb_upper`, `bb_lower`, `rsi14`(如已算), `z_score`(如已算) |
| SuperTrendScalper | `supertrend_value`, `supertrend_dir`(1/-1), `last_close` |
| OrderBookScalper | `best_bid`, `best_ask`, `imbalance`(期货侧,子代理暂不用但保留) |

- 若某策略一个指标都没算,v1 允许 `{}`,Phase 4 统一补

### 3.4 main.cpp 接线(两处修改 + 一处构造)

1. **创建板**(`apps/pulsetrader/main.cpp:914` 之前):

```cpp
auto board = std::make_shared<pulse::strategy::SignalBoard>(
    cfg.strategy.signal_aggregator_threshold);
```

2. **策略信号回调**(main.cpp:914-919)改为:

```cpp
strategy_mgr.setSignalCallback(
    [&aggregator, board](const pulse::strategy::TradingSignal &sig)
    {
        board->publish(sig);        // 原始信号 + 指标快照进因子板
        aggregator.addSignal(sig);
    });
```

3. **聚合输出回调**(main.cpp:1049-1053)改为:

```cpp
aggregator.setOutputCallback(
    [&order_flow, board](const pulse::strategy::TradingSignal &sig)
    {
        board->publishAggregate(sig);   // 两种模式都发布,复盘可审计
        order_flow.onSignal(sig);       // 闸门在 OrderFlowExecutor 内部
    });
```

4. **EngineServices 构造**(main.cpp:1063 附近)传入 board(见 §3.6-1)

### 3.5 OrderFlowExecutor:signal_only 闸门

- `OrderFlowExecutor` 构造函数**已接收 `cfg.strategy`**(main.cpp:972-989 第一个参数)→ 存 `bool m_signalOnly`
- `onSignal()` 入口处:`if (m_signalOnly) { return; }`(可选:低频 debug 日志记录"信号已发布但未执行")
- **闸门放在这里而不是 main 回调**,原因:(a) `OrderFlowTest` 可直接测试;(b) 统一覆盖所有信号入口;(c) 手动 `openOrder` 是不同入口,天然不受影响
- 加 `[[nodiscard]] bool signalOnly() const;` 访问器供测试断言
- ⚠️ 请核实 `onSignal` 里是否有除下单外的必要副作用(如统计计数);若有,保留副作用、只短路下单部分

### 3.6 `get_signals` 控制平面方法(四通道)

1. **EngineServices**(`src/control/EngineServices.hpp`):
   - ctor 增加参数 `strategy::SignalBoard &signalBoard`(或 shared_ptr,建议引用保持与其它组件一致)
   - 新查询方法:`[[nodiscard]] nlohmann::json signals() const;` → `m_signalBoard.snapshot()`
   - 更新 main.cpp 与 `EngineServicesTest` 的构造点
2. **JsonRpcServer**(`src/control/JsonRpcServer.cpp`,注册表参考 371-447 行的 `reg["get_market"]`):

```cpp
reg["get_signals"] = [&services](const nlohmann::json &)
{
    return services.signals();
};
```

3. **McpServer::toolDefinitions()**(`src/control/McpServer.cpp:56` 的 `add` 模式):

```cpp
add("get_signals",
    "Latest per-strategy signals + indicator snapshots + aggregate consensus "
    "from the signal board. Entries older than ~120s should be treated as "
    "stale by consumers.",
    nlohmann::json::object(), {});
```

> ⚠️ M14 教训:inputSchema 的 required 用**数组**,禁止嵌套布尔 `"required": true`(McpServer.cpp:40-41 有注释)

4. **CommandParser**:REPL 新命令 `signals`(无参数,参考 218 行 `market` 命令模式)→ `ParsedCommand{ "get_signals", {} }`;输出用既有 `renderTable` 渲染 source/type/confidence/age
5. **ControlClient**:若有类型化方法列表,补 `signals()`(实施时核实)

### 3.7 `place_trigger_order` 方法(交易所侧保护止损,P0 必备)

- **目的**:子代理开仓后立即在 Gate 交易所侧挂止损触发单——引擎、子代理全挂也能止损(解决"止损只存在于子代理轮询里"的裸仓风险)
- **实现依据**:`0_note/gate交易/Gate期货触发单TP-SL管理技术备忘_2026-08-16.md`(price_orders 接口 + sign_req 函数,含 7 条踩坑,实施前必读)
- **方法**:`EngineServices::placeTriggerOrder(const nlohmann::json &params)`
  - params:`{ symbol, side("buy"|"sell"), quantity, trigger_price, market_type, trigger_kind("stop_loss"|"take_profit"|"trigger") }`
  - 返回:订单响应。⚠️ tradfi 下单**不回显订单 id** → 复用 M17 `matchCfdOrderId` 的模式从 open-orders 列表解析 id(见 `src/execution/OrderExecutor.cpp`)
  - 通道:JSON-RPC + MCP tool(最低要求这两个;REPL 可选)
- 若范围冲突可拆为 Phase 2b,但**子代理开第一笔实盘仓之前必须完成**

### 3.8 测试清单(目标:687 现有全绿 + 新增 ~15-20)

| 测试文件 | 用例 |
|---|---|
| `SignalBoardTest`(新) | publish 按 strategy_id 覆盖最新;snapshot 结构;aggregate 槽位;空板快照;并发 publish+snapshot 冒烟(shared_mutex) |
| 既有 `signal_types` 测试 | TradingSignal 默认 `indicators == {}` |
| `OrderFlowTest` | `SignalOnlySkipsExecution`(signal_only=true → onSignal 后无订单/无预留);`SignalOnlyManualOrderStillWorks`(手动 openOrder 不受影响) |
| config loader/validator 测试 | `signal_only` true/false/缺省 false |
| `JsonRpcServerTest` | get_signals 注册、返回 board JSON |
| `McpServerTest` | tools/list 含 get_signals;调用链路返回正常 |
| `CommandParserTest` | `"signals"` 解析 |
| `EngineServicesTest` | `signals()` 与注入 board 联动 |

### 3.9 文档同步(实施会话顺手做)

- `AGENTS.md`:方法列表 16→17(+place_trigger_order 则 18)
- `OPERATIONAL_GUIDE.md`:控制平面一节加 get_signals / REPL `signals`
- `README.md`(如列方法):同步
- `.claude/project-memory.md`:新增 M18 小节(本设计落地摘要)+ 更新测试计数(687→N)
- `trading.toml`:`[strategy]` 下加 `signal_only = false`(附注释,默认关保持现状)

### 3.10 验收标准(Definition of Done)

- [ ] 全部测试绿(687 + 新增)
- [ ] `signal_only=true` 启动:XAUUSD 策略运行中、`get_signals` 持续有数据、观察 ≥10 分钟零新订单
- [ ] `signal_only=false` 回归:行为与现网一致(测试覆盖)
- [ ] 手动 `open_order` 在 signal_only=true 下仍可用
- [ ] MCP `tools/list` 出现 `get_signals`;REPL `signals` 有输出
- [ ] 提交风格沿用惯例(feat/fix/docs 前缀,测试计数入提交信息)

---

## 4. 子代理 v2 决策模型(因子决策)

> 这是 `xauusd-subagent-strategy.md` 的 v2 升级,落地后回填该文件。因子来源:SignalBoard(`get_signals`)+ 子代理自算(`get_market` klines/ticker)。**因子新鲜度 ≤120s 才计入**。

**Gate 层(任一不满足 → 不动)**:

| 因子 | 条件 |
|---|---|
| session | ∈ {london, ny}(初版可放宽为工作日 10:00–21:00 UTC;⚠️ 交易时段/滚期/周末时间以 docs/CFD_TRADFI.md 与实测为准) |
| volatility_regime | ≠ hot(ATR14(1m) ≤ 3×近 20 根中位数;Phase 4 落地前由子代理自算) |
| 冷却 | 止损/连亏后 2 根 K 线内不开新仓;子代理与引擎聚合信号方向冲突时观望 |
| 日风控 | 当日已实现 > -8 USD、连亏 < 2、当日 < 4 笔往返(账户 37~45 USD 口径) |
| 仓位 | 子代理最多 1 个 CFD 持仓;有仓则只做出场管理 |

**方向过滤(必选)**:

- `ema_separation` 同向且 |分离| > 0.3×ATR(粘合期不开;Phase 4 落地前子代理用 klines 自算)
- 增强项(任一):引擎聚合信号同向,或 ≥2 个策略信号同向且各自新鲜度达标

**触发(≥1)**:

- RSI14 极值(<35 多 / >65 空,与方向一致)
- 价格贴 S-R:距 swing 高/低点 < 0.5×ATR 且未破位(ticker 确认)
- 突破:1m 收盘破近 20 根区间高/低(ticker 确认,防假突破)

**出场**:

| 规则 | 触发 | 动作 |
|---|---|---|
| 硬止损 | -5 USD | 平仓(交易所侧触发单兜底,§3.7) |
| 止盈 | +8~10 USD | 兑现 |
| 因子反向 | 聚合/ema_separation 反转 | 提前平仓 |
| 时间止损 | 持仓 >60 分钟且浮盈 < 1 USD | 平价或小亏出 |
| 滚期 | 每日维护窗口前 ≥15 分钟 | 清仓(防 swap 与跳空) |

**执行与记账**:

- 开仓:`open_order`(market_type=cfd, type=market, 0.01)——开仓后**立即**挂交易所侧保护止损(`place_trigger_order`)
- 平仓:`close_position`(position_id 来自 get_positions)
- 状态落盘:`0_note/gate交易/黄金/joey-Z170I-PRO-GAMING/状态/xauusd-agent-state.json`(日期/当日已实现/连亏/冷却截止/持仓快照)——交接与重启不丢
- 每笔复盘:`0_note/gate交易/黄金/joey-Z170I-PRO-GAMING/复盘/xauusd-review-YYYYMMDD-HHMMSS.md`;汇总指标进 `xauusd-stats.md`(胜率/期望/MAE/MFE/点差样本);周度门禁:3 红日或 20 笔期望 <0 → 暂停改规则
- 交接协议:报告必含持仓状态 + 止损计划 + 最后检查时间;恢复后**第一动作先查持仓健康,再开新仓**

### §4.1 M22 修订(2026-08-18,回合 130→132 实盘教训:RSI 64.66 近超买追多突破,尖刺 TR 2.83 打穿 SL,滑点 0.77,已实现 -5.53,账户 10.62→5.09 直接停手)

**规则层新增(任务书已同步)**:

1. **超买/超卖对冲突破过滤**:RSI > 65 不追多破位 / RSI < 35 不追空破位;破例需增强+方向双双更强(⚖️ 门槛 |分离| ≥ 0.5×ATR 且聚合新鲜同向)
2. **尖刺风险预检**:入场前查近 20 根 maxTR;maxTR > SL 距离(5 点)→ 记录预期滑点;maxTR > 2×SL 距离(10 点)→ 跳过该入场
3. **资金厚度门槛(闸 6)**:可用 − 新仓保证金 < 2×止损金额(-10 USD)不交易

**代码层新增(引擎 M22,759 测试绿)**:

4. **外部平仓留痕**:sync_positions 清幽灵仓时合成 ExecutionReport(`ext_close_` 前缀)+ 写 trades.db + 日志,补全审计链(覆盖交易所侧 SL/TP 触发与用户 App 手动平仓——f36d8cd 只覆盖引擎主动平仓)
5. **止损后最小可用资金闸**:`risk.minAvailableAfterStopUsd`(默认关);CFD 带 SL 开仓要求 可用 − 保证金 − 止损金额 ≥ 该值,否则 3009 InsufficientFreeMargin 拒绝

---

## 5. Phase 4 因子工具规范(扩展点)

> 不在 Phase 2 范围,但接口按此预留,保证"新增工具 = 新增因子,子代理接口零变更"。

**接口草案**(后续实施,文件建议 `src/strategy/signal/FactorEngine.hpp`):

```cpp
class FactorSource
{
  public:
    virtual ~FactorSource() = default;
    virtual std::string id() const = 0;      // "factor:volatility_regime" 等
    virtual void onKline(const Kline &k, MarketType mt) = 0;   // 或 onTicker
    virtual void publish(SignalBoard &board) = 0;              // 写 FactorEntry(source = id())
};
// FactorEngine:持有 SignalBoard&,注册 FactorSource,从 MarketFeed 订阅事件分发
```

**首批工具规格**:

| 工具 id | 输入 | 逻辑 | 输出 | 频率 | 优先级 |
|---|---|---|---|---|---|
| `factor:volatility_regime` | 1m K 线 | ATR14 vs 近 20 根中位数 → calm/normal/hot | indicators: {atr, median, regime} | 每根收盘 | 高(§4 Gate 依赖) |
| `factor:ema_separation` | 1m K 线 | (EMA9−EMA21)/ATR14 | {ema9, ema21, separation} | 每根收盘 | 高(§4 方向过滤依赖) |
| `factor:sr_levels` | 1m K 线 | swing N=3 高低点,最近 100 根;最近支撑/阻力与距离 | {support, resistance, dist_s, dist_r} | 每根收盘 | 高 |
| `factor:spread` | CFD ticker | ask−bid 滚动中位数(parseCfdTicker 已有 bid_price/ask_price,MarketFeed.cpp:524) | {spread, median_60s} | 每秒 | 中(成本样本) |
| `factor:session` | 静态表 | UTC 时刻 → asia/london/ny/rollover/closed | {session} | 每分钟 | 高(时段表须实测核实) |
| `factor:ticker_momentum` | ticker | 近 60s 价格变化 / ATR14 | {delta, delta_atr} | 每秒 | 中(入场确认) |

- 每个工具 = 一个 FactorSource 注册 + SignalBoard 一条 entry + 2~3 个测试
- 工具只读,绝不下单;下单永远只经子代理

---

## 6. 过渡与部署

**Phase 1(立即,任何会话可做,纯运行时操作,不改码)**:

- 暂停 3 个 XAUUSD 策略(当前它们**仍在实盘运行**):MCP `pause_strategy` ×3 或 REPL `pause <id>`,id 为:
  - `momentum_scalper_XAUUSD` / `mean_reversion_scalper_XAUUSD` / `supertrend_scalper_XAUUSD`
- ⚠️ 运行时暂停**重启即失效**(main.cpp:1080 启动时按 `active_market` 自动恢复 cfd 策略)——这只是过渡安全网

**Phase 2**:按 §3 实施 → 重建 → 测试 → 提交 → 文档同步(§3.9)

**Phase 3**:

1. `trading.toml` 设 `signal_only = true` → 重启引擎(重启后 XAUUSD 策略自动恢复运行,但在 signal-only 下只发信号)
2. 按 §3.10 验收:观察 ≥10 分钟零新订单、get_signals 有数据、手动路径可用
3. 子代理 v2(§4)上线;`place_trigger_order` 就绪后方可开第一笔实盘仓
4. 回填 `xauusd-subagent-strategy.md` 至 v2

**Phase 4**:按 §5 逐个添加因子工具(每批独立提交)

**回滚**:`signal_only = false` + 重启 = 完全回到现状;组件删除是干净的(新文件独立,无侵入既有路径)

---

## 7. 实施会话需核实的开放事项

1. 各策略内部**已计算的指标变量名**(§3.3 表格按此落地;v1 只发现成的)
2. `StrategyBase::emitSignal` 是否也发 `Flat` 信号?(影响因子板覆盖率;若只发 Buy/Sell,子代理靠 120s 新鲜度过滤即可,不必改)
3. `OrderFlowExecutor` 手动入口的函数名与 `onSignal` 的边界(§3.5 闸门放置依据;`src/control/OrderFlowExecutor.cpp`)
4. `JsonRpcServer` 注册表的精确位置与闭包捕获方式(`src/control/JsonRpcServer.cpp:371-447` 的 `reg[...]` 模式)
5. `McpServer` 的 dispatch 机制(工具名→方法转发?照现有工具加一个即可;`src/control/McpServer.cpp:56` 起)
6. `ControlClient` 是否有需要同步的类型化方法列表
7. `EngineServicesTest` 的构造点更新(`src/control/` 测试目录)
8. Gate TradFi XAUUSD **交易时段/每日维护窗口/滚期时间**(`docs/CFD_TRADFI.md`,§4 session 因子与时间止损依赖;不确定就实测记录)
9. 提交时同步 `.claude/project-memory.md`(M18 小节 + 测试计数)
10. 若实施期间其它会话已提交,先 `git pull` 再动工,行号以最新代码为准

---

## 8. 参考索引

| 文件 | 用途 |
|---|---|
| `0_note/gate交易/Gate期货触发单TP-SL管理技术备忘_2026-08-16.md` | price_orders + sign_req 实现(§3.7 必读) |
| `0_note/gate交易/黄金/joey-Z170I-PRO-GAMING/策略/xauusd-subagent-strategy.md` | 子代理规则基线 v1(本文 §4 为其 v2) |
| repo `docs/CFD_TRADFI.md` | CFD API 调查、交易时段 |
| repo `.claude/project-memory.md` | 控制平面方法列表、M14-M17 历史、OrderFlowTest 回归清单 |
| repo `OPERATIONAL_GUIDE.md` | 控制平面文档(§3.9 同步目标) |
| repo `src/strategy/signal_types.hpp` / `signal/SignalAggregator.{hpp,cpp}` | 信号类型与聚合器(本设计的挂载点) |
