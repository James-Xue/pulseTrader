# MomentumScalper — EMA crossover momentum strategy

> A classic dual moving-average crossover trend-following strategy that determines the trend direction from the relative position of a fast and a slow EMA.

## Core Principle

### Indicator definition

**EMA (Exponential Moving Average)** is a weighted moving average that gives more weight to recent prices:

```
EMA = price × k + prev_ema × (1 - k)
k = 2 / (period + 1)
```

- **Fast EMA** (9 periods by default): more sensitive to price changes
- **Slow EMA** (21 periods by default): smoother with respect to price changes

### Signal logic

| Event | Condition | Signal |
|------|------|------|
| Bullish crossover | Fast EMA crosses above the slow EMA | Buy |
| Bearish crossover | Fast EMA crosses below the slow EMA | Sell |

### Confidence calculation

```
confidence = clamp(|fast_ema - slow_ema| / slow_ema, 0.0, 1.0)
```

- The further apart the two EMAs are, the stronger the trend
- High confidence means a clear trend; low confidence means an unclear one

---

## Implementation Details

### Data requirements

- **Data source**: kline close price (`onKline()` callback)
- **Minimum kline count**: `ema_slow_period + 1` (22 bars by default)
- **Polling interval**: 200ms (configured in `trading.toml`)

### Warm-up mechanism

On first startup, enough kline data must be accumulated:

```
if (candles.size() < slow_period) {
    // Print warm-up progress once every 30 seconds
    PULSE_LOG_INFO("Warming up: {}/{} candles accumulated", 
        candles.size(), slow_period);
    return;
}
```

Warm-up time depends on the kline period:
- 1-minute klines: ~21 minutes
- 5-minute klines: ~105 minutes

### EMA calculation

1. **First calculation**: use the SMA of the first `period` klines as the seed value
2. **Subsequent calculations**: update with the recurrence formula

```cpp
// SMA seed
double ema = sum(first N closes) / period;

// EMA update
for each new close:
    ema = close * k + ema * (1 - k);
```

### Crossover detection

The previous EMA values must be retained:

```cpp
bool bullish_cross = (prev_fast <= prev_slow) && (curr_fast > curr_slow);
bool bearish_cross = (prev_fast >= prev_slow) && (curr_fast < curr_slow);
```

---

## Parameter Configuration

### Strategy parameters (StrategyParams)

| Parameter | Type | Default | Description |
|------|------|--------|------|
| `ema_fast_period` | int | 9 | Fast line period |
| `ema_slow_period` | int | 21 | Slow line period |
| `cooldown_seconds` | double | 30.0 | Signal cooldown time |

### trading.toml example

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

## Pros and Cons Analysis

### Pros

1. **Clear logic**: a crossover is an unambiguous trend-turning signal
2. **Noise resistant**: the EMA itself is smooth, so a single abnormal kline does not disturb it
3. **Strong trend capture**: once a trend forms, it keeps following it until the reversal
4. **Simple parameters**: only 2 period parameters, easy to understand and tune

### Cons

1. **Lag**: EMA is a lagging indicator, so the trend has already moved by the time the crossover occurs
   - The best entry point may already have passed when entering
   - Part of the profit may already have been given back when exiting
2. **Poor performance in ranging markets**: frequent false crossovers when sideways, causing "sawtooth losses"
   - Remedy: filter with a trend-strength indicator such as ADX
3. **Low signal frequency**: must wait for a complete crossover; there may be no signal for a long time
4. **Cannot catch sharp reversals**: on a V-shaped reversal the crossover signal comes too late

---

## Applicable Scenarios

### Best market conditions

- **One-directional trending market**: daily-scale uptrends or downtrends in BTC/ETH
- **Medium-low volatility**: avoids frequent false signals caused by extreme moves
- **Sufficient liquidity**: ensures continuous kline data with no large gaps

### Unsuitable markets

- **Sideways range**: EMAs cross repeatedly, producing many false signals
- **High-volatility gaps**: gaps distort the EMA calculation
- **Low liquidity**: kline data is discontinuous and signal quality degrades

---

## Tuning Recommendations

### Increase sensitivity (more signals)

```
ema_fast_period = 5   # shorten the fast line
ema_slow_period = 13  # shorten the slow line
```

- More frequent signals
- Earlier entries but more false signals
- Suited to short-term trading

### Increase stability (fewer signals)

```
ema_fast_period = 12  # lengthen the fast line
ema_slow_period = 26  # lengthen the slow line
```

- Fewer but more reliable signals
- More noticeable lag
- Suited to medium-to-long-term trends

### Adaptive parameters

Adjust dynamically according to market volatility:

```cpp
// Lengthen the periods in high volatility, shorten them in low volatility
int adjusted_fast = base_fast * (1 + volatility_factor);
int adjusted_slow = base_slow * (1 + volatility_factor);
```

---

## Combining with Other Strategies

### Combining with OrderBookScalper

- MomentumScalper determines the overall direction
- OrderBookScalper looks for micro entry points in the direction of the trend
- Effect: precise entry after the trend is confirmed

### Combining with MeanReversionScalper

- In trending markets MomentumScalper leads
- In ranging markets MeanReversionScalper leads
- Requires market-state detection logic to switch between them

### Combining with SuperTrendScalper

- Both are trend following, so they can cross-validate each other
- SuperTrend's ATR adaptivity complements the EMA's fixed periods
- Confidence is higher when they emit signals at the same time

---

## Source Locations

- **Header**: `src/strategy/scalping/MomentumScalper.hpp`
- **Implementation**: `src/strategy/scalping/MomentumScalper.cpp`
- **Parameter definitions**: `src/strategy/StrategyParams.hpp`

---

## Log Examples

```
[INFO] [strategy] [momentum_scalper_ETH_USDT] EMA bullish crossover (fast > slow) signal: 
       confidence=0.0023, price=3456.78
[INFO] [strategy] [momentum_scalper_ETH_USDT] Warming up: 15/21 candles accumulated 
       (need ~21 min of kline data)
```
