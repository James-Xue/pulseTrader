# MomentumScalper — EMA 交叉动量策略

> 经典的双均线交叉趋势跟踪策略，通过快慢 EMA 的相对位置判断趋势方向。

## 核心原理

### 指标定义

**EMA (Exponential Moving Average)** 是加权移动平均线，近期价格权重更大：

```
EMA = price × k + prev_ema × (1 - k)
k = 2 / (period + 1)
```

- **Fast EMA**（默认 9 周期）：对价格变化更敏感
- **Slow EMA**（默认 21 周期）：对价格变化更平滑

### 信号逻辑

| 事件 | 条件 | 信号 |
|------|------|------|
| 金叉（Bullish Crossover） | Fast EMA 从下穿上 Slow EMA | Buy |
| 死叉（Bearish Crossover） | Fast EMA 从上穿下 Slow EMA | Sell |

### 置信度计算

```
confidence = clamp(|fast_ema - slow_ema| / slow_ema, 0.0, 1.0)
```

- 两条 EMA 距离越远，说明趋势越强
- 置信度高表示趋势明确，低表示趋势不明

---

## 实现细节

### 数据需求

- **数据源**：K 线收盘价（`onKline()` 回调）
- **最少 K 线数**：`ema_slow_period + 1`（默认 22 根）
- **轮询间隔**：200ms（在 `trading.toml` 中配置）

### 预热机制

首次启动时需要积累足够的 K 线数据：

```
if (candles.size() < slow_period) {
    // 每 30 秒打印一次预热进度
    PULSE_LOG_INFO("Warming up: {}/{} candles accumulated", 
        candles.size(), slow_period);
    return;
}
```

预热时间取决于 K 线周期：
- 1 分钟 K 线：需要 ~21 分钟
- 5 分钟 K 线：需要 ~105 分钟

### EMA 计算

1. **首次计算**：用前 `period` 根 K 线的 SMA 作为种子值
2. **后续计算**：用递推公式更新

```cpp
// SMA seed
double ema = sum(first N closes) / period;

// EMA update
for each new close:
    ema = close * k + ema * (1 - k);
```

### 交叉检测

需要保存上一轮的 EMA 值：

```cpp
bool bullish_cross = (prev_fast <= prev_slow) && (curr_fast > curr_slow);
bool bearish_cross = (prev_fast >= prev_slow) && (curr_fast < curr_slow);
```

---

## 参数配置

### 策略参数（StrategyParams）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `ema_fast_period` | int | 9 | 快线周期 |
| `ema_slow_period` | int | 21 | 慢线周期 |
| `cooldown_seconds` | double | 30.0 | 信号冷却时间 |

### trading.toml 示例

```toml
[[strategy.instances]]
name            = "momentum_scalper"
symbol          = "ETH_USDT"
market_type     = "futures"
leverage        = 5
margin_mode     = "cross"
order_quantity  = 1
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 200
```

---

## 优缺点分析

### 优点

1. **逻辑清晰**：交叉点是明确的趋势转折信号
2. **抗噪声**：EMA 本身有平滑效果，不会被单根异常 K 线干扰
3. **趋势捕捉能力强**：一旦趋势形成，会持续跟随直到反转
4. **参数简单**：只有 2 个周期参数，容易理解和调优

### 缺点

1. **滞后性**：EMA 是滞后指标，交叉发生时趋势已经走了一段
   - 入场时可能已经错过最佳位置
   - 出场时可能已经回吐部分利润
2. **震荡市表现差**：横盘时频繁假交叉，导致"锯齿亏损"
   - 解决方案：配合 ADX 等趋势强度指标过滤
3. **信号频率低**：需要等待完整交叉，可能长时间无信号
4. **无法捕捉急速反转**：V 型反转时交叉信号来得太晚

---

## 适用场景

### 最佳市场条件

- **单边趋势市场**：BTC/ETH 日线级别的上涨或下跌趋势
- **中低波动率**：避免极端波动导致的频繁假信号
- **足够流动性**：确保 K 线数据连续，没有大量跳空

### 不适合的市场

- **横盘震荡**：EMA 反复交叉，产生大量假信号
- **高波动率跳空**：跳空会导致 EMA 计算失真
- **低流动性**：K 线数据不连续，信号质量下降

---

## 调优建议

### 提高灵敏度（更多信号）

```
ema_fast_period = 5   # 缩短快线
ema_slow_period = 13  # 缩短慢线
```

- 信号更频繁
- 入场更早但假信号更多
- 适合短线交易

### 提高稳定性（更少信号）

```
ema_fast_period = 12  # 延长快线
ema_slow_period = 26  # 延长慢线
```

- 信号更少但更可靠
- 滞后更明显
- 适合中长线趋势

### 自适应参数

根据市场波动率动态调整：

```cpp
// 高波动率时拉长周期，低波动率时缩短
int adjusted_fast = base_fast * (1 + volatility_factor);
int adjusted_slow = base_slow * (1 + volatility_factor);
```

---

## 与其他策略的配合

### 与 OrderBookScalper 配合

- MomentumScalper 判断大方向
- OrderBookScalper 在趋势方向上寻找微观入场点
- 效果：趋势确认后精确入场

### 与 MeanReversionScalper 配合

- 趋势市时 MomentumScalper 主导
- 震荡市时 MeanReversionScalper 主导
- 需要市场状态判断逻辑切换

### 与 SuperTrendScalper 配合

- 两者都是趋势跟踪，可以交叉验证
- SuperTrend 的 ATR 自适应特性可以补充 EMA 的固定周期
- 同时发出信号时置信度更高

---

## 源码位置

- **头文件**：`src/strategy/scalping/MomentumScalper.hpp`
- **实现文件**：`src/strategy/scalping/MomentumScalper.cpp`
- **参数定义**：`src/strategy/StrategyParams.hpp`

---

## 日志示例

```
[INFO] [strategy] [momentum_scalper_ETH_USDT] EMA bullish crossover (fast > slow) signal: 
       confidence=0.0023, price=3456.78
[INFO] [strategy] [momentum_scalper_ETH_USDT] Warming up: 15/21 candles accumulated 
       (need ~21 min of kline data)
```
