# EmaResonanceScalper — 五周期 EMA 共振策略

> 五条 EMA(默认 7/14/30/60/200)严格全排列对齐才出信号的趋势共振策略。
> 只在"共振状态迁移"瞬间入场,不追不重发。

## 核心原理

### 指标定义

五条指数移动平均线(EMA),周期默认 `7 / 14 / 30 / 60 / 200`:

| 周期 | 角色 | 敏感度 |
|------|------|--------|
| 7  | 最快线 | 对价格变化最敏感 |
| 14 | 次快线 | 短线节奏 |
| 30 | 中线   | 中期趋势 |
| 60 | 次慢线 | 中期趋势确认 |
| 200 | 最慢线 | 长期趋势基准 |

EMA 递推公式(`computeEma`,k = 2/(period+1)):每根新 K 线用快照内前 `period` 根收盘价的 SMA 播种后全量递推 —— **确定性计算,无滚动状态漂移**。

### 共振定义(全排列对齐)

| 状态 | 条件 | 意义 |
|------|------|------|
| **bull_aligned** | `EMA7 > EMA14 > EMA30 > EMA60 > EMA200`(严格递减排列) | 全周期多头共振 |
| **bear_aligned** | `EMA7 < EMA14 < EMA30 < EMA60 < EMA200`(严格递增排列) | 全周期空头共振 |
| **mixed** | 其余任何情况 | 无共振,不出信号 |

五条线同向叠放 = 趋势在所有时间尺度上达成共识,是**最强但最稀缺**的信号。

### 信号逻辑(状态迁移触发)

信号只在**共振状态发生变化**的瞬间发出,不是每根 K 线重发:

| 状态迁移 | 信号 | 方向 |
|----------|------|------|
| mixed → bull_aligned | Buy | 做多 |
| bear_aligned → bull_aligned(直接翻转) | Buy | 做多 |
| mixed → bear_aligned | Sell | 做空 |
| bull_aligned → bear_aligned(直接翻转) | Sell | 做空 |
| 保持同一种对齐不变 | 无信号 | —— |

- **不强追**:对齐状态持续期间不重复发信号(模板 cooldown 是第二道独立闸门)
- **不抢跑**:预热期(默认 201 根 K 线)内不评估
- 每次评估都无条件提交当前共振状态(趋势已变就是已变)

### 置信度计算

```
confidence = clamp( |EMA7 − EMA200| / ATR14 × res_conf_scale, 0, 1 )
```

- 分子 = 共振堆的**总跨度**(最快线到最慢线),堆越宽趋势共识越强
- 分母 = ATR14 波动率归一化:同样的跨度在平静市场得高分,在剧烈震荡市得低分
- `res_conf_scale` 默认 1.0,可整体缩放。主网实例已用 2.0:首条真信号实测
  |ema7-ema200|/ATR=0.5459,乘 1.0 不过 min_confidence=0.6 闸被 emitSignal
  静默丢弃(2026-08-23 实盘验证发现,与 eth_scalper scale 20 同族)

---

## 实现细节

### 数据需求

- **数据源**:K 线收盘价(`onKline()` 回调)
- **最少 K 线数**:`klineNeeded = 最慢周期 + 1`(默认 201)
- **预热**:`warmupThreshold = 201` —— 最慢 EMA 的 SMA 播种需要 200 根 + 当前 1 根;引擎启动时 KlineBuffer 回填 500 根,实际预热只需几秒

### EMA 全量重算

每次评估对快照内全部收盘价调用 `computeEma(closes, period, 0.0)`:
- `prev = 0.0` 触发 SMA 播种(快照内前 `period` 根均值)→ 全量递推
- 五条线独立、无状态 → 结果只取决于 K 线序列,确定性强
- 201 根 × 5 条,1 分钟 K 线频率下开销可忽略

### 共振状态机

```
evaluateEntry:
  1. 读 5 个周期参数(custom_params, 静态)
  2. 提取 closes → 5×computeEma 全量重算
  3. 严格全序判定 → Bull / Bear / None
  4. 迁移检测: m_hasPrev && res != prev && res != None
  5. 无条件提交 m_prevResonance(先提交,再判断信号——cooldown 不撤销已提交状态)
  6. 有迁移 + ATR>0 → 组装 Buy/Sell + 置信度 + indicators
  7. 无迁移 → nullopt(状态已提交)
```

### indicators 快照(随真实信号发布)

```json
{ "ema7": 100.625, "ema14": 100.333, "ema30": 100.161,
  "ema60": 100.082, "ema200": 100.025,
  "resonance": "bull_aligned", "atr": 1.143 }
```

> 键名 `ema7/14/30/60/200` 按默认周期命名;若通过 custom_params 改周期,值跟随 `res_ema_p1..p5` 的配置,键名不变。

---

## 参数配置

### custom_params(静态,TOML 实例级,无热更新)

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `res_ema_p1` | 7 | 最快 EMA 周期 |
| `res_ema_p2` | 14 | 次快 EMA 周期 |
| `res_ema_p3` | 30 | 中线 EMA 周期 |
| `res_ema_p4` | 60 | 次慢 EMA 周期 |
| `res_ema_p5` | 200 | 最慢 EMA 周期 |
| `res_conf_scale` | 1.0 | 置信度缩放系数 |

> 与 eth_scalper 的币种参数同一渠道:静态、构造时读取。若要 MCP/AI 热调,需迁移到 StrategyParams + paramSetters/paramGetters 白名单(见 README 架构说明)。

### trading.toml 实例

```toml
[[strategy.instances]]
name            = "ema_resonance_scalper"
symbol          = "ETH_USDT"
market_type     = "futures"
leverage        = 10
margin_mode     = "cross"
order_quantity  = 20
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 500
custom_params = { res_ema_p1 = 7, res_ema_p2 = 14, res_ema_p3 = 30, res_ema_p4 = 60, res_ema_p5 = 200, res_conf_scale = 2.0 }
```

### 下单模式(先信号后激活)

策略代码具备完整直接下单能力(Buy/Sell 信号经正常聚合器路径 → 风控闸门)。但引擎全局开关决定是否真的下单:

- 当前引擎 `signal_only = true` + `active_market = "cfd"` → 本策略**只发信号到信号板**,不下单
- 以后要激活自动交易:翻转 `signal_only = false` 且 `active_market = "futures"`(注意:这会同时激活 BTC/SNDK/UNITREE 等所有 enabled 策略的自动下单)

---

## 优缺点分析

### 优点

1. **信号质量极高**:五条 EMA 全排列对齐是趋势的强共识,天然过滤震荡市假信号
2. **状态机清晰**:迁移触发 + 不重发 + cooldown 双闸门,行为可预期
3. **置信度可解释**:堆跨度 / 波动率,直观反映"共识强度"
4. **实现确定性强**:全量重算无滚动状态,便于测试与回放
5. **参数与代码分离**:规则文本(本文档)↔ C++ 实现(源码)一一对应

### 缺点

1. **信号极其稀少**:全排列对齐要求苛刻,单边大趋势才触发;震荡市可能长期无信号
2. **滞后性**:EMA 是滞后指标,200 周期最慢线决定入场偏晚,趋势末段进场风险高
3. **无止损止盈联动**:策略只负责信号,风控(仓位上限于风险闸门)与出场需要外部配合
4. **跳空敏感**:大幅跳空会让快慢线瞬时拉开,可能触发对齐(置信度公式已用 ATR 归一化对冲一部分)

---

## 适用场景

### 最佳市场条件

- **单边大趋势**:日线/4h 级别趋势确认后的顺势入场
- **趋势中继**:深度回调后重新全排列对齐 = 趋势延续信号
- **低波动上升**:跨周期共识明确、ATR 收敛时置信度高

### 不适合的市场

- **宽幅震荡**:五线反复交叉纠缠,几乎无法全排列对齐(但也因此几乎没有假信号)
- **急拉急砸**:暴拉/插针瞬间快慢线可能瞬时对齐,需配合 spike 过滤(见扩展点)

---

## 与现有策略的配合

| 组合 | 用法 |
|------|------|
| 与 MomentumScalper(9/21) | 共振策略做**趋势确认**,动量策略做**快线时机**,两者同向信号置信度更高 |
| 与 eth_scalper(追空) | 空头共振对齐可为追空网格提供更严格的趋势闸门(需扩展 GridManager 的 readTrendGate 消费源) |
| 与 SuperTrendScalper | 同为趋势跟踪,可交叉验证;共振对齐 + SuperTrend 翻转同向 = 强信号 |

---

## 扩展点(v1 未实现)

1. **per-candle 状态发布**:eth_scalper v2 式每根 K 线发布 Flat 状态供网格作趋势闸门 —— 注意生产配置下 `emitSignal` 的 min_confidence 门会丢弃 conf=0 的 Flat 信号,需 min_confidence=0 或改 emitSignal 加 Flat 豁免
2. **spike 过滤**:暴拉/插针过滤(可复用 eth_scalper 的三口径 filter 模式)
3. **热调周期参数**:迁入 StrategyParams + MCP/AI 通道后可在线调参

---

## 源码位置

- **头文件**:`src/strategy/scalping/EmaResonanceScalper.hpp`
- **实现文件**:`src/strategy/scalping/EmaResonanceScalper.cpp`
- **基类**:`src/strategy/scalping/UnifiedScalper.hpp`(模板方法 + computeEma/computeAtr)
- **注册**:`src/strategy/StrategyRegistry.cpp`(`ema_resonance_scalper`)
- **单测**:`tests/unit/strategy/test_ema_resonance_scalper.cpp`

---

## 日志示例

```
[INFO] [strategy] [ema_resonance_scalper_ETH_USDT] EMA resonance bullish alignment (all 5 EMAs ordered) signal: confidence=0.5251, price=102.50
[INFO] [strategy] [ema_resonance_scalper_ETH_USDT] EMA resonance bearish alignment (all 5 EMAs ordered) signal: confidence=0.5251, price=90.00
[INFO] [strategy] [ema_resonance_scalper_ETH_USDT] Warming up: 200/201 candles accumulated
```
