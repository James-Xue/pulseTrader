# OrderBookScalper — 订单簿失衡策略

> 通过分析订单簿买卖盘深度失衡，捕捉短期价格方向性变动。

## 核心原理

### 订单簿失衡指标

**Imbalance** 衡量买卖盘压力的不平衡程度：

```
imbalance = (bid_volume - ask_volume) / (bid_volume + ask_volume)
```

- `imbalance > 0`：买盘压力大于卖盘（看涨）
- `imbalance < 0`：卖盘压力大于买盘（看跌）
- `imbalance ∈ [-1.0, 1.0]`：归一化后的值

### 信号逻辑

| 条件 | 信号 | 含义 |
|------|------|------|
| `imbalance > threshold` | Buy | 买盘显著强于卖盘，价格可能上涨 |
| `imbalance < -threshold` | Sell | 卖盘显著强于买盘，价格可能下跌 |

### 置信度

```
confidence = clamp(|imbalance|, 0.0, 1.0)
```

失衡越严重，信号置信度越高。

---

## 实现细节

### 数据需求

- **数据源**：订单簿深度快照（`onOrderbook()` 回调）
- **深度层数**：默认分析前 5 档买卖盘
- **轮询间隔**：100ms（最快的策略）

### 计算步骤

```cpp
// 1. 累加前 N 档买盘量
double bid_volume = 0;
for (int i = 0; i < depth; ++i) {
    bid_volume += book.bids[i].quantity;
}

// 2. 累加前 N 档卖盘量
double ask_volume = 0;
for (int i = 0; i < depth; ++i) {
    ask_volume += book.asks[i].quantity;
}

// 3. 计算失衡度
double imbalance = (bid_volume - ask_volume) / (bid_volume + ask_volume);
```

### 冷却机制

避免同一方向的信号过于密集：

```cpp
auto nowMs = current_time_ms();
if (nowMs - m_lastSignalTimeMs < cooldown_ms) {
    return; // 冷却期内不发信号
}
```

默认冷却时间 30 秒。

### 价格选择

- **Buy 信号**：使用 `book.bids[0].price`（最佳买价）
- **Sell 信号**：使用 `book.asks[0].price`（最佳卖价）

这确保信号价格是当前市场可执行的最优价格。

---

## 参数配置

### 策略参数（StrategyParams）

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `ob_depth` | int | 5 | 分析的订单簿深度层数 |
| `ob_imbalance_threshold` | double | 0.3 | 失衡阈值（0.0-1.0） |
| `cooldown_seconds` | double | 30.0 | 信号冷却时间 |

### trading.toml 示例

```toml
[[strategy.instances]]
name            = "orderbook_scalper"
symbol          = "ETH_USDT"
market_type     = "futures"
leverage        = 5
margin_mode     = "cross"
order_quantity  = 1
min_confidence  = 0.65
enabled         = true
poll_interval_ms = 100
```

---

## 优缺点分析

### 优点

1. **信号领先**：订单簿压力是价格变动的先行指标
   - 大额买单堆积 → 价格即将上涨
   - 大额卖单堆积 → 价格即将下跌
2. **信号频率高**：每次订单簿更新都可以评估
   - 适合高频交易
   - 捕捉微观价格变动
3. **无需预热**：只要有深度数据即可工作
   - 启动后立即可以产生信号
   - 不像 K 线策略需要等待数据积累
4. **低延迟**：100ms 轮询，响应速度快

### 缺点

1. **假信号多**：订单簿可以被快速撤销（Spoofing）
   - 大单可能是诱饵，不会真正成交
   - 需要配合成交量确认
2. **深度数据噪音大**：订单簿变化频繁
   - 需要合理设置阈值过滤
   - 阈值太低 → 假信号多
   - 阈值太高 → 信号太少
3. **不适合趋势市**：只看微观压力，忽略宏观方向
   - 强趋势中可能逆势交易
   - 需要配合趋势指标
4. **对数据质量依赖高**：需要稳定的 WS 连接
   - WS 断线时无法工作
   - 深度数据延迟会导致信号过时

---

## 适用场景

### 最佳市场条件

- **高流动性市场**：BTC/ETH 等主流币合约
- **震荡市**：价格在小区间内波动，订单簿压力有效
- **低波动率**：避免极端波动导致的深度数据失真
- **稳定 WS 连接**：确保订单簿数据实时准确

### 不适合的市场

- **低流动性**：深度数据稀疏，信号不可靠
- **强趋势市**：订单簿压力无法预测趋势延续
- **极端波动**：闪崩或暴涨时深度数据混乱
- **WS 不稳定**：频繁断线导致数据缺失

---

## 调优建议

### 提高灵敏度（更多信号）

```
ob_depth = 3                    # 只看前 3 档
ob_imbalance_threshold = 0.2   # 降低阈值
cooldown_seconds = 15          # 缩短冷却
```

- 信号更频繁
- 假信号更多
- 适合超短线剥头皮

### 提高稳定性（更少信号）

```
ob_depth = 10                   # 看前 10 档
ob_imbalance_threshold = 0.5   # 提高阈值
cooldown_seconds = 60          # 延长冷却
```

- 信号更少但更可靠
- 响应更慢
- 适合稳健交易

### 深度加权

不同档位的权重不同（近档更重要）：

```cpp
double weighted_bid = 0;
for (int i = 0; i < depth; ++i) {
    double weight = 1.0 / (i + 1); // 第 1 档权重 1.0，第 2 档 0.5，...
    weighted_bid += book.bids[i].quantity * weight;
}
```

---

## 与其他策略的配合

### 与 MomentumScalper 配合

- MomentumScalper 判断大趋势方向
- OrderBookScalper 只在趋势方向上交易
- 过滤逆势信号，提高胜率

### 与 MeanReversionScalper 配合

- MeanReversionScalper 识别超买/超卖
- OrderBookScalper 确认微观压力方向
- 双重确认反转信号

### 与 SuperTrendScalper 配合

- SuperTrend 判断趋势状态
- 趋势中用 MomentumScalper，震荡中用 OrderBookScalper
- 需要市场状态切换逻辑

---

## 源码位置

- **头文件**：`src/strategy/scalping/OrderBookScalper.hpp`
- **实现文件**：`src/strategy/scalping/OrderBookScalper.cpp`
- **参数定义**：`src/strategy/StrategyParams.hpp`

---

## 日志示例

```
[INFO] [strategy] [orderbook_scalper_ETH_USDT] Order book buy pressure 
       (imbalance > threshold) signal: imbalance=0.4523, bid_vol=12.3456, ask_vol=3.7890
[INFO] [strategy] [orderbook_scalper_ETH_USDT] Order book sell pressure 
       (imbalance < -threshold) signal: imbalance=-0.3876, bid_vol=5.1234, ask_vol=15.6789
```
