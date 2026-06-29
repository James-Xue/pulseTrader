# SuperTrendScalper — ATR 趋势翻转策略

> 基于 SuperTrend 指标的趋势跟踪策略，利用 ATR 自适应波动率捕捉趋势翻转。

## 核心原理

### SuperTrend 指标

SuperTrend 是一个趋势跟踪指标，由以下部分组成：

```
ATR = Average True Range（平均真实波幅）
基本上轨 = (High + Low) / 2 + multiplier × ATR
基本下轨 = (High + Low) / 2 - multiplier × ATR
```

### 轨道收紧逻辑

SuperTrend 的轨道只会向有利方向移动：

- **上轨**：只降不升（除非价格突破上轨）
- **下轨**：只升不降（除非价格跌破下轨）

这使得 SuperTrend 线成为动态支撑/阻力位。

### 趋势判断

```
if (close >= lower_band) → 看涨趋势，SuperTrend = lower_band
if (close < lower_band) → 看跌趋势，SuperTrend = upper_band
```

### 信号逻辑

| 事件 | 条件 | 信号 |
|------|------|------|
| 趋势翻多 | 从看跌翻转为看涨 | Buy |
| 趋势翻空 | 从看涨翻转为看跌 | Sell |

### 置信度计算

```
confidence = clamp(|close - supertrend| / atr, 0.0, 1.0)
```

- 价格偏离 SuperTrend 越远，趋势越强
- 用 ATR 归一化，适应不同波动率

---

## 实现细节

### 数据需求

- **数据源**：K 线 High/Low/Close（`onKline()` 回调）
- **最少 K 线数**：`supertrend_period + 1`（默认 11 根）
- **轮询间隔**：500ms

### ATR 计算

**True Range (TR)**：

```
TR = max(High - Low, |High - prev_Close|, |Low - prev_Close|)
```

**ATR**：

```
ATR = sum(last N TRs) / N
```

```cpp
double computeAtr(const std::vector<Kline>& candles, size_t period) {
    double sum_tr = 0.0;
    for (size_t i = candles.size() - period; i < candles.size(); ++i) {
        double hl = candles[i].high - candles[i].low;
        double hpc = std::abs(candles[i].high - candles[i-1].close);
        double lpc = std::abs(candles[i].low - candles[i-1].close);
        sum_tr += std::max({hl, hpc, lpc});
    }
    return sum_tr / period;
}
```

### 轨道收紧实现

```cpp
// 上轨收紧
double final_upper = basic_upper;
if (basic_upper < prev_upper || prev_close > prev_upper) {
    final_upper = basic_upper; // 重置
} else {
    final_upper = prev_upper;  // 保持
}

// 下轨收紧
double final_lower = basic_lower;
if (basic_lower > prev_lower || prev_close < prev_lower) {
    final_lower = basic_lower; // 重置
} else {
    final_lower = prev_lower;  // 保持
}
```

### 趋势翻转检测

```cpp
bool flipped_bullish = !prev_bullish && current_bullish;
bool flipped_bearish = prev_bullish && !current_bullish;

if (flipped_bullish) {
    emitSignal(Buy);
} else if (flipped_bearish) {
    emitSignal(Sell);
}
```

### 预热机制

```cpp
if (candles.size() < needed) {
    // 每 30 秒打印一次预热进度
    PULSE_LOG_INFO("Warming up: {}/{} candles accumulated", 
        candles.size(), needed);
    return;
}
```

预热时间：
- 1 分钟 K 线：需要 ~11 分钟
- 5 分钟 K 线：需要 ~55 分钟

---

## 参数配置

### 策略参数（StrategyParams）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `supertrend_period` | int | 10 | ATR 周期 |
| `supertrend_multiplier` | double | 3.0 | ATR 倍数 |
| `cooldown_seconds` | double | 30.0 | 信号冷却时间 |

### trading.toml 示例

```toml
[[strategy.instances]]
name            = "supertrend_scalper"
symbol          = "ETH_USDT"
market_type     = "futures"
leverage        = 5
margin_mode     = "cross"
order_quantity  = 1
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 500
```

---

## 优缺点分析

### 优点

1. **自适应波动率**：ATR 自动调整带宽
   - 高波动率时带宽增加，避免假信号
   - 低波动率时带宽收窄，提高灵敏度
2. **信号较及时**：趋势翻转时立即触发
   - 不像 EMA 交叉有明显滞后
   - 翻转点是明确的趋势转折点
3. **止损清晰**：SuperTrend 线本身可作为动态止损位
   - 看涨时止损设在 SuperTrend 下方
   - 看跌时止损设在 SuperTrend 上方
4. **参数鲁棒**：multiplier 在较大范围内都有效
   - 2.0-4.0 都能工作
   - 不像布林带对 std_dev 敏感

### 缺点

1. **计算复杂**：需要 H/L/C 数据和多步计算
   - 比 EMA/布林带更复杂
   - 需要保存多个状态变量
2. **震荡市假信号**：价格在带附近反复穿越
   - 横盘时频繁翻转
   - 需要配合趋势强度过滤
3. **预热稍长**：需要 period+1 根 K 线
   - 比 OrderBookScalper 慢
   - 但比 MomentumScalper 快
4. **对跳空敏感**：大幅跳空可能导致 ATR 异常
   - 跳空时 TR 会很大
   - 可能导致带宽突然扩大

---

## 适用场景

### 最佳市场条件

- **趋势/波动率上升**：SuperTrend 的自适应特性发挥最佳
- **中等波动率**：避免极端波动导致的假信号
- **明确趋势**：单边趋势中 SuperTrend 效果最好
- **足够流动性**：确保 K 线数据连续

### 不适合的市场

- **横盘震荡**：价格反复穿越 SuperTrend，假信号多
- **极端波动**：闪崩或暴涨时 ATR 失真
- **低流动性**：K 线数据不连续，ATR 计算不准确
- **重大事件**：新闻驱动的行情不受技术面约束

---

## 调优建议

### 提高灵敏度（更多信号）

```
supertrend_period = 7       # 缩短周期
supertrend_multiplier = 2.0 # 降低倍数
```

- 信号更频繁
- 带宽更窄，更容易翻转
- 假信号更多

### 提高稳定性（更少信号）

```
supertrend_period = 14      # 延长周期
supertrend_multiplier = 4.0 # 提高倍数
```

- 信号更少但更可靠
- 带宽更宽，翻转更罕见
- 滞后更明显

### 动态倍数

根据市场状态调整倍数：

```cpp
double dynamic_multiplier = base_multiplier;
if (trend_strength > 0.7) {
    dynamic_multiplier *= 0.8; // 强趋势时收紧
} else {
    dynamic_multiplier *= 1.2; // 弱趋势时放宽
}
```

### 配合趋势过滤

在震荡市中禁用 SuperTrend：

```cpp
if (adx < 20) {
    // 弱趋势，禁用 SuperTrend 策略
    return;
}
```

---

## 与其他策略的配合

### 与 MomentumScalper 配合

- 两者都是趋势跟踪，可以交叉验证
- 同时发出信号时置信度更高
- SuperTrend 的 ATR 自适应补充 EMA 的固定周期

### 与 OrderBookScalper 配合

- SuperTrend 判断趋势方向
- OrderBookScalper 在趋势方向上寻找微观入场点
- 趋势确认后精确入场

### 与 MeanReversionScalper 配合

- 趋势市时 SuperTrend 主导
- 震荡市时 MeanReversionScalper 主导
- 需要市场状态判断逻辑切换

---

## SuperTrend 作为止损位

SuperTrend 线本身是优秀的动态止损位：

```cpp
// 看涨持仓
stop_loss = current_supertrend - buffer * atr;

// 看跌持仓
stop_loss = current_supertrend + buffer * atr;
```

- `buffer` 通常设为 0.5-1.0
- 止损位随趋势移动，锁定利润

---

## 源码位置

- **头文件**：`src/strategy/scalping/SuperTrendScalper.hpp`
- **实现文件**：`src/strategy/scalping/SuperTrendScalper.cpp`
- **参数定义**：`src/strategy/StrategyParams.hpp`

---

## 日志示例

```
[INFO] [strategy] [supertrend_scalper_ETH_USDT] SuperTrend flipped bullish 
       (price crossed above band) signal: confidence=0.4523, price=3478.90, atr=12.34
[INFO] [strategy] [supertrend_scalper_ETH_USDT] SuperTrend flipped bearish 
       (price crossed below band) signal: confidence=0.3876, price=3445.20, atr=11.89
[INFO] [strategy] [supertrend_scalper_ETH_USDT] Warming up: 8/11 candles accumulated 
       (need ~11 min of kline data)
```
