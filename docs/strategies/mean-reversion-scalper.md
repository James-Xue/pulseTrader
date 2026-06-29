# MeanReversionScalper — 布林带均值回归策略

> 利用布林带识别超买/超卖区域，在价格偏离均值时反向交易。

## 核心原理

### 布林带（Bollinger Bands）

布林带由三条线组成：

```
中轨（SMA）= 过去 N 根 K 线的收盘价平均值
上轨 = SMA + k × 标准差
下轨 = SMA - k × 标准差
```

- **SMA**：简单移动平均线（中轨）
- **标准差**：衡量价格波动程度
- **k**：标准差倍数（默认 2.0）

### 信号逻辑

| 条件 | 信号 | 含义 |
|------|------|------|
| `price <= lower_band` | Buy | 价格触及下轨，超卖，预期反弹 |
| `price >= upper_band` | Sell | 价格触及上轨，超买，预期回落 |

### 置信度计算

```
confidence = clamp(|price - band| / band_width, 0.0, 1.0)
```

- 价格偏离轨道越远，置信度越高
- `band_width = upper_band - lower_band`

---

## 实现细节

### 数据需求

- **数据源**：K 线收盘价（`onKline()` 回调）
- **最少 K 线数**：`bb_period`（默认 20 根）
- **轮询间隔**：500ms

### 计算步骤

```cpp
// 1. 提取收盘价
std::vector<double> closes;
for (const auto& c : candles) {
    closes.push_back(c.close);
}

// 2. 计算 SMA
double sum = std::accumulate(closes.begin(), closes.end(), 0.0);
double sma = sum / closes.size();

// 3. 计算标准差
double sq_sum = 0.0;
for (double price : closes) {
    double diff = price - sma;
    sq_sum += diff * diff;
}
double stddev = std::sqrt(sq_sum / closes.size());

// 4. 计算布林带
double upper_band = sma + bb_std_dev * stddev;
double lower_band = sma - bb_std_dev * stddev;
```

### 预热机制

```cpp
if (candles.size() < bb_period) {
    // 每 30 秒打印一次预热进度
    PULSE_LOG_INFO("Warming up: {}/{} candles accumulated", 
        candles.size(), bb_period);
    return;
}
```

预热时间：
- 1 分钟 K 线：需要 ~20 分钟
- 5 分钟 K 线：需要 ~100 分钟

### 冷却机制

```cpp
auto nowMs = current_time_ms();
if (nowMs - m_lastSignalTimeMs < cooldown_ms) {
    return; // 冷却期内不发信号
}
```

---

## 参数配置

### 策略参数（StrategyParams）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `bb_period` | int | 20 | 布林带周期 |
| `bb_std_dev` | double | 2.0 | 标准差倍数 |
| `cooldown_seconds` | double | 30.0 | 信号冷却时间 |

### trading.toml 示例

```toml
[[strategy.instances]]
name            = "mean_reversion_scalper"
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

1. **逆势交易**：在超买/超卖极端位置入场
   - 捕捉价格回归均值的利润
   - 适合震荡市
2. **胜率较高**：价格大概率会向均值回归
   - 统计上 95% 的价格在 2 倍标准差内
   - 突破轨道是相对罕见的事件
3. **信号明确**：触碰布林带是清晰的触发条件
   - 不需要复杂的交叉判断
   - 信号容易理解和验证
4. **适合震荡市**：大部分时间市场处于震荡区间
   - 横盘时布林带效果最好
   - 提供明确的买卖点

### 缺点

1. **趋势市亏损大**：单边趋势中价格持续突破带外
   - "沿着轨道走"（walking the band）
   - 逆势加仓会导致巨大亏损
2. **逆势加仓风险**：需要严格止损配合
   - 不能无限加仓摊平成本
   - 必须设置最大回撤限制
3. **信号频率中等**：需要等待价格触及上下轨
   - 不是每次都会突破轨道
   - 可能长时间无信号
4. **参数敏感**：std_dev 倍数影响信号质量
   - 倍数太低 → 信号太多，假信号多
   - 倍数太高 → 信号太少，错过机会

---

## 适用场景

### 最佳市场条件

- **横盘震荡**：价格在区间内波动，布林带效果最佳
- **中等波动率**：波动太小信号少，波动太大假信号多
- **无明显趋势**：避免单边趋势市场
- **足够流动性**：确保 K 线数据连续

### 不适合的市场

- **强趋势市**：价格持续单边，突破轨道后不回归
- **极端波动**：闪崩或暴涨时布林带失效
- **低流动性**：K 线数据不连续，标准差计算失真
- **重大事件**：新闻驱动的行情不受技术面约束

---

## 调优建议

### 提高灵敏度（更多信号）

```
bb_period = 15      # 缩短周期
bb_std_dev = 1.5   # 降低标准差倍数
```

- 信号更频繁
- 轨道更窄，更容易突破
- 假信号更多

### 提高稳定性（更少信号）

```
bb_period = 30      # 延长周期
bb_std_dev = 2.5   # 提高标准差倍数
```

- 信号更少但更可靠
- 轨道更宽，突破更罕见
- 胜率更高

### 动态带宽

根据市场波动率调整标准差倍数：

```cpp
double dynamic_std_dev = base_std_dev * (1 + volatility_factor);
// 高波动率时放宽轨道，低波动率时收紧轨道
```

### 配合趋势过滤

在趋势市中禁用均值回归：

```cpp
if (adx > 25) {
    // 强趋势，禁用均值回归策略
    return;
}
```

---

## 与其他策略的配合

### 与 MomentumScalper 配合

- MomentumScalper 判断趋势状态
- 趋势市时禁用 MeanReversionScalper
- 震荡市时启用 MeanReversionScalper

### 与 OrderBookScalper 配合

- MeanReversionScalper 识别超买/超卖区域
- OrderBookScalper 确认微观压力方向
- 双重确认反转信号

### 与 SuperTrendScalper 配合

- SuperTrend 判断趋势方向
- 趋势方向与均值回归方向相反时，优先趋势
- 趋势不明时，均值回归主导

---

## 源码位置

- **头文件**：`src/strategy/scalping/MeanReversionScalper.hpp`
- **实现文件**：`src/strategy/scalping/MeanReversionScalper.cpp`
- **参数定义**：`src/strategy/StrategyParams.hpp`

---

## 日志示例

```
[INFO] [strategy] [mean_reversion_scalper_ETH_USDT] Price at/below lower Bollinger Band 
       (oversold, mean reversion expected) signal: price=3420.15, upper=3480.50, 
       lower=3425.30, sma=3452.90
[INFO] [strategy] [mean_reversion_scalper_ETH_USDT] Price at/above upper Bollinger Band 
       (overbought, mean reversion expected) signal: price=3510.80, upper=3495.20, 
       lower=3440.60, sma=3467.90
```
