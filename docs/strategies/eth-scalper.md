# EthScalper — ETH 追空 EMA 交叉策略(币种专属)

> ETH 专属只做空策略:EMA bearish cross 触发 Sell,配合三口径暴拉过滤与
> ATR 自适应止盈提示。v2 起每根 K 线发布趋势状态(Flat + trend_state/spike)
> 供网格子代理作趋势闸门 —— 首个展示 UnifiedScalper 币种扩展机制的示范策略。

## 核心原理

### 指标定义

双 EMA(热可调,StrategyParams 原子量):

| 周期 | 默认 | 角色 |
|------|------|------|
| `ema_fast_period` | 9 | 快线,交叉触发线 |
| `ema_slow_period` | 21 | 慢线,趋势确认线 |

### 只做空语义(追空 regime)

| 事件 | 信号 |
|------|------|
| **bearish cross**(fast 从 ≥ slow 跌破 slow)且非 spike | **Sell**(真实信号) |
| bullish cross(fast 上穿 slow) | 无信号 —— **绝不追多** |

ETH 为追空币种:只吃空头交叉,多头交叉一律忽略。

### 暴拉过滤(三口径,v2 复盘升级)

`range = high − low`(单根 1m K 线振幅),三口径任一触发即判为 spike:

| 口径 | 默认 | 触发条件 |
|------|------|----------|
| `eth_spike_filter_usd` | 120 | range > 120 USD |
| `eth_spike_filter_pct` | 1.5 | range > 1.5% × close |
| `eth_spike_filter_atr` | 3.0 | range > 3.0 × ATR14 |

spike 时**不追空**(bearish cross + spike → 只发 Flat 状态,reason 注明 "spike filter tripped")。
任一口径设 0 即禁用该口径。

> 复盘教训(eth-review-20260821-grid-v1.md):v1 只有 USD 单口径,挡不住 04:50
> 1m +4.4% 暴拉(低价 K 线 USD 振幅小,但百分比/ATR 口径能捕获)—— 三口径是
> 网格开局 20 格全成交事故的固化防线。

### 趋势状态发布(v2,2026-08-21)

每根 K 线**无条件**发布一个状态条目:

- `type = flat`、`confidence = 0`(非交易信号,是状态通道)
- indicators 恒带 `trend_state`(bullish / bearish / neutral)+ `spike`(0/1)

用途:网格子代理/GridManager 的趋势闸门 —— **挂格前置:trend_state=bearish 且
spike=0 才允许挂新格**,消费方无需自算 EMA。

> ⚠️ 2026-08-23 实盘验证发现:`emitSignal` 的 min_confidence 闸会丢弃 conf=0 的
> Flat 信号(日志先于闸打印,板上永远无条目)—— 已在 `StrategyManager.cpp` 加
> Flat 豁免(见 git 4aacbe0),聚合器本就忽略 Flat,放行无交易风险。

### 置信度计算

```
confidence = clamp( |ema_fast − ema_slow| / ATR14 × eth_min_confidence_scale, 0, 1 )
```

- 分子 = 双线间距,间距越大交叉越有力
- 分母 = ATR14 波动率归一化
- 主网实例 `eth_min_confidence_scale = 20.0`:实测 |EMA9-EMA21|/ATR ≈ 0.03,
  乘 1.0 永远过不了 min_confidence=0.6 闸,真信号被静默丢弃(2026-08-23 实盘
  发现,与 ema_resonance res_conf_scale=2.0 同族问题);20 使典型交叉 ~0.65~1.0
  过闸,仍守 0.6 全局语义

### ATR 自适应止盈提示

```
suggested_tp = close − eth_atr_step × ATR14
```

策略只出**提示价**(indicators 字段),不挂单 —— 大波动给宽止盈,小波动给紧止盈。

---

## 实现细节

### 数据需求

- **数据源**:K 线收盘价(`onKline()` 回调,1m)
- **最少 K 线数**:`klineNeeded = max(slow_period + 1, 15)`(慢线 SMA 播种 + ATR14 需要)
- **预热**:`warmupThreshold = slow_period`(默认 21 根)

### 状态机

```
evaluateEntry:
  1. 读热参数(fast/slow period)+ 静态币种参数(custom_params)
  2. 提取 closes → 双 EMA(prev 播种 = 快照 SMA,确定性)
  3. trend_state:fast<slow=bearish / fast>slow=bullish / 相等=neutral
  4. spike:三口径 range 判定,任一触发=spike
  5. bearish_cross: m_hasPrev && (prevFast>=prevSlow) && (fast<slow)
  6. 无条件提交 m_prevEmaFast/Slow(交叉已发生就是已发生,cooldown 不撤销)
  7. bearish_cross && !spike && atr>0 → Sell + 置信度
  8. 否则 → Flat 状态条目(trend_state + spike 恒带)
```

### indicators 快照(随 Flat/Sell 发布)

```json
{ "ema_fast": 2417.78, "ema_slow": 2417.93,
  "trend_state": "bearish", "atr": 1.29, "atr_step": 0.05,
  "suggested_tp": 2417.82, "spike": 0,
  "spike_range_usd": 0.28, "spike_range_pct": 0.012,
  "spike_filter_usd": 120.0, "spike_filter_pct": 1.5, "spike_filter_atr": 3.0 }
```

---

## 参数配置

### 热参数(StrategyParams,MCP/REPL 可调)

| 参数 | 默认 | 说明 |
|------|------|------|
| `ema_fast_period` | 9 | 快线周期 |
| `ema_slow_period` | 21 | 慢线周期 |
| `min_confidence` | 0.6 | 发射闸(emitSignal);主网实例保持 0.6 全局语义 |
| `order_quantity` | 20 | 合约数(2026-08-23 起从 TOML 播种,此前显示 0.001) |

### custom_params(静态,TOML 实例级,无热更新)

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `eth_atr_step` | 0.05 | 止盈提示距离倍数(suggested_tp = close − step×ATR) |
| `eth_spike_filter_usd` | 120.0 | 单根 K 线振幅超此 USD 值 = spike |
| `eth_spike_filter_pct` | 1.5 | 振幅超 close 的此百分比 = spike |
| `eth_spike_filter_atr` | 3.0 | 振幅超此 ×ATR = spike |
| `eth_min_confidence_scale` | 1.0 | 置信度缩放(主网 20.0,见上) |

### trading.toml 实例

```toml
[[strategy.instances]]
name            = "eth_scalper"
symbol          = "ETH_USDT"
market_type     = "futures"
leverage        = 10
margin_mode     = "cross"
order_quantity  = 20
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 500
custom_params = { eth_spike_filter_usd = 120, eth_spike_filter_pct = 1.5, eth_spike_filter_atr = 3.0, eth_atr_step = 0.05, eth_min_confidence_scale = 20.0 }
```

> **BTC_USDT 同名实例(2026-09-04)**:类为 symbol-agnostic(无 ETH 硬编码,实例
> id 自动为 `eth_scalper_BTC_USDT`),加实例即可。**USD 口径必须按价量级重标**:
> `eth_spike_filter_usd=120` 在 ETH(~$2500)≈ 4.8% 的极端单根;照搬到 $81k 的
> BTC 等于 0.15%,正常波动全会误判 spike → BTC 版放宽为 **2000(≈2.5%)**;
> pct/atr/conf_scale 为无量纲口径沿用。初值上线后按信号板 confidence/spike
> 实测再调(与 ETH 08-23 标定流程相同)。示例:

```toml
[[strategy.instances]]
name            = "eth_scalper"
symbol          = "BTC_USDT"
market_type     = "futures"
leverage        = 10
margin_mode     = "cross"
order_quantity  = 1              # 1 张 = 0.0001 BTC(信息性,signal_only)
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 500
custom_params = { eth_spike_filter_usd = 2000, eth_spike_filter_pct = 1.5, eth_spike_filter_atr = 3.0, eth_atr_step = 0.05, eth_min_confidence_scale = 20.0 }
```

### 下单模式(先信号后激活)

引擎全局 `signal_only = true` → 本策略只发信号到信号板,不下单。激活自动交易:
翻转 `signal_only` 或对实例 `autotrade ... on`(M28 一键开关)。重启回仅信号(fail-safe)。

---

## 优缺点分析

### 优点

1. **只做空纪律明确**:追空 regime 下绝不接多,语义单一可预期
2. **状态通道独立于交易**:Flat 条目让消费方随时读到趋势闸门,不依赖交叉事件
3. **三口径暴拉过滤**:USD/百分比/ATR 互补,任何价格量级都能防插针
4. **确定性计算**:prev 播种全量重算,无滚动状态漂移,便于测试回放
5. **币种扩展示范**:完整展示 UnifiedScalper 三类扩展机制(自定义入场/状态发布/币种参数)

### 缺点

1. **滞后性**:EMA 交叉天然滞后,趋势末段追空风险高
2. **无止损止盈执行**:只出 suggested_tp 提示,保护需外部(风控闸/代理/网格)落实
3. **震荡市反复交叉**:区间震荡下交叉频繁,虽无假信号但状态噪音多
4. **置信度语义需缩放**:原始 |EMA间距|/ATR 量级 ~0.03,与全局 0.6 闸不匹配,
   依赖 scale 系数(主网 20.0)—— 改参数时须同步理解这层映射

---

## 适用场景

### 最佳市场条件

- **明确下行趋势**:EMA 持续 bearish 排列,回调后的 bearish cross = 顺势追空点
- **网格空头挂单前置**:trend_state=bearish 且 spike=0 时挂格/加格(ETH 网格 v2 协议)

### 不适合的市场

- **单边强多**:bullish cross 被忽略,本策略完全静默(状态仍发布,方向=多)
- **剧烈插针行情**:spike 过滤会挡住大部分交叉(这正是设计目的)

---

## 与现有策略的配合

| 组合 | 用法 |
|------|------|
| 与 ema_resonance_scalper | 空头共振对齐(bear_aligned)可为追空提供更强的趋势确认;两者 bearish 同向 = 双确认 |
| 与 GridManager(M27) | 状态条目是引擎内网格趋势闸门的数据源(readTrendGate);挂格前置 trend_state=bearish && spike=0 |
| 与动量/均线策略 | 通用策略信号 + eth_scalper 状态可交叉验证 ETH 方向共识 |

---

## 扩展点(v2 未实现)

1. **多头开关**:加 custom_params 开关允许 bullish cross 发 Buy(当前追空 regime 硬编码)
2. **入场价建议**:suggested_entry 类似 suggested_tp,给代理/网格更完整参考
3. **热调币种参数**:custom_params 迁入 StrategyParams + paramSetters 白名单后可 MCP/AI 在线调
4. **多币种复用**:参数表已通用,可为其他追空币种建同名实例(StrategyRegistry 注册一行即可)

---

## 源码位置

- **头文件**:`src/strategy/scalping/EthScalper.hpp`
- **实现文件**:`src/strategy/scalping/EthScalper.cpp`
- **基类**:`src/strategy/scalping/UnifiedScalper.hpp`(模板方法 + computeEma/computeAtr)
- **注册**:`src/strategy/StrategyRegistry.cpp`(`eth_scalper`)
- **单测**:`tests/unit/strategy/test_eth_scalper.cpp`

---

## 日志示例

```
[INFO] [strategy] [eth_scalper_ETH_USDT] ETH short setup: EMA bearish crossover (fast < slow) signal: confidence=1.0000, price=2412.56
[INFO] [strategy] [eth_scalper_ETH_USDT] ETH state: EMA trend bearish — no signal signal: confidence=0.0000, price=2412.67
[INFO] [strategy] [eth_scalper_ETH_USDT] ETH state: bearish crossover but spike filter tripped (range > usd/pct/atr threshold) — no chase
[INFO] [strategy] [eth_scalper_ETH_USDT] Warming up: 14/21 candles accumulated (need ~21 min of kline data)
```
