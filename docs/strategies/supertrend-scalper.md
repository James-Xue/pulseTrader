# SuperTrendScalper — ATR trend-flip strategy

> A trend-following strategy based on the SuperTrend indicator, using ATR-adaptive volatility to capture trend flips.

## Core Principle

### The SuperTrend indicator

SuperTrend is a trend-following indicator made up of the following parts:

```
ATR = Average True Range
Basic upper band = (High + Low) / 2 + multiplier × ATR
Basic lower band = (High + Low) / 2 - multiplier × ATR
```

### Band tightening logic

SuperTrend bands only move in the favorable direction:

- **Upper band**: only falls, never rises (unless price breaks above the upper band)
- **Lower band**: only rises, never falls (unless price breaks below the lower band)

This makes the SuperTrend line a dynamic support/resistance level.

### Trend determination

```
if (close >= lower_band) → bullish trend, SuperTrend = lower_band
if (close < lower_band) → bearish trend, SuperTrend = upper_band
```

### Signal logic

| Event | Condition | Signal |
|------|------|------|
| Trend flips bullish | Flips from bearish to bullish | Buy |
| Trend flips bearish | Flips from bullish to bearish | Sell |

### Confidence calculation

```
confidence = clamp(|close - supertrend| / atr, 0.0, 1.0)
```

- The further price deviates from SuperTrend, the stronger the trend
- Normalizing by ATR adapts to different volatility levels

---

## Implementation Details

### Data requirements

- **Data source**: kline High/Low/Close (`onKline()` callback)
- **Minimum kline count**: `supertrend_period + 1` (11 bars by default)
- **Polling interval**: 500ms

### ATR calculation

**True Range (TR)**:

```
TR = max(High - Low, |High - prev_Close|, |Low - prev_Close|)
```

**ATR**:

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

### Band tightening implementation

```cpp
// Tighten the upper band
double final_upper = basic_upper;
if (basic_upper < prev_upper || prev_close > prev_upper) {
    final_upper = basic_upper; // reset
} else {
    final_upper = prev_upper;  // keep
}

// Tighten the lower band
double final_lower = basic_lower;
if (basic_lower > prev_lower || prev_close < prev_lower) {
    final_lower = basic_lower; // reset
} else {
    final_lower = prev_lower;  // keep
}
```

### Trend-flip detection

```cpp
bool flipped_bullish = !prev_bullish && current_bullish;
bool flipped_bearish = prev_bullish && !current_bullish;

if (flipped_bullish) {
    emitSignal(Buy);
} else if (flipped_bearish) {
    emitSignal(Sell);
}
```

### Warm-up mechanism

```cpp
if (candles.size() < needed) {
    // Print warm-up progress once every 30 seconds
    PULSE_LOG_INFO("Warming up: {}/{} candles accumulated", 
        candles.size(), needed);
    return;
}
```

Warm-up time:
- 1-minute klines: ~11 minutes
- 5-minute klines: ~55 minutes

---

## Parameter Configuration

### Strategy parameters (StrategyParams)

| Parameter | Type | Default | Description |
|------|------|--------|------|
| `supertrend_period` | int | 10 | ATR period |
| `supertrend_multiplier` | double | 3.0 | ATR multiplier |
| `cooldown_seconds` | double | 30.0 | Signal cooldown time |

### trading.toml example

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

## Pros and Cons Analysis

### Pros

1. **Adaptive volatility**: ATR automatically adjusts the band width
   - In high volatility the band widens, avoiding false signals
   - In low volatility the band narrows, increasing sensitivity
2. **Timely signals**: triggers immediately when the trend flips
   - Unlike EMA crossovers, there is no obvious lag
   - The flip point is an unambiguous trend turning point
3. **Clear stop-loss**: the SuperTrend line itself can serve as a dynamic stop-loss level
   - In a bullish position the stop-loss is set below SuperTrend
   - In a bearish position the stop-loss is set above SuperTrend
4. **Robust parameters**: the multiplier is effective across a wide range
   - Anything from 2.0 to 4.0 works
   - Unlike Bollinger Bands, it is not sensitive to std_dev

### Cons

1. **Complex computation**: needs H/L/C data and a multi-step calculation
   - More complex than EMA/Bollinger Bands
   - Requires keeping several state variables
2. **False signals in ranging markets**: price repeatedly crosses back and forth around the band
   - Frequent flips when the market is sideways
   - Needs to be combined with trend-strength filtering
3. **Slightly longer warm-up**: needs period+1 kline bars
   - Slower than OrderBookScalper
   - But faster than MomentumScalper
4. **Sensitive to gaps**: a large gap can produce an abnormal ATR
   - TR is very large when there is a gap
   - This can suddenly widen the band

---

## Applicable Scenarios

### Best market conditions

- **Trend / rising volatility**: SuperTrend's adaptive behavior works best
- **Medium volatility**: avoids false signals caused by extreme moves
- **Clear trend**: SuperTrend performs best in one-directional trends
- **Sufficient liquidity**: ensures continuous kline data

### Unsuitable markets

- **Sideways range**: price repeatedly crosses SuperTrend, producing many false signals
- **Extreme volatility**: ATR is distorted during flash crashes or spikes
- **Low liquidity**: kline data is discontinuous and the ATR calculation is inaccurate
- **Major events**: news-driven moves are not bound by technicals

---

## Tuning Recommendations

### Increase sensitivity (more signals)

```
supertrend_period = 7       # shorten the period
supertrend_multiplier = 2.0 # lower the multiplier
```

- More frequent signals
- Narrower bands, easier to flip
- More false signals

### Increase stability (fewer signals)

```
supertrend_period = 14      # lengthen the period
supertrend_multiplier = 4.0 # raise the multiplier
```

- Fewer but more reliable signals
- Wider bands, flips are rarer
- More noticeable lag

### Dynamic multiplier

Adjust the multiplier according to the market state:

```cpp
double dynamic_multiplier = base_multiplier;
if (trend_strength > 0.7) {
    dynamic_multiplier *= 0.8; // tighten in a strong trend
} else {
    dynamic_multiplier *= 1.2; // widen in a weak trend
}
```

### Combine with trend filtering

Disable SuperTrend in ranging markets:

```cpp
if (adx < 20) {
    // Weak trend: disable the SuperTrend strategy
    return;
}
```

---

## Combining with Other Strategies

### Combining with MomentumScalper

- Both are trend following, so they can cross-validate each other
- Confidence is higher when they emit signals at the same time
- SuperTrend's ATR adaptivity complements the EMA's fixed period

### Combining with OrderBookScalper

- SuperTrend determines the trend direction
- OrderBookScalper looks for micro entry points in the direction of the trend
- Enter precisely once the trend is confirmed

### Combining with MeanReversionScalper

- In trending markets SuperTrend leads
- In ranging markets MeanReversionScalper leads
- Requires market-state detection logic to switch between them

---

## SuperTrend as a Stop-Loss Level

The SuperTrend line itself is an excellent dynamic stop-loss level:

```cpp
// Bullish position
stop_loss = current_supertrend - buffer * atr;

// Bearish position
stop_loss = current_supertrend + buffer * atr;
```

- `buffer` is usually set to 0.5-1.0
- The stop-loss level moves with the trend, locking in profit

---

## Source Locations

- **Header**: `src/strategy/scalping/SuperTrendScalper.hpp`
- **Implementation**: `src/strategy/scalping/SuperTrendScalper.cpp`
- **Parameter definitions**: `src/strategy/StrategyParams.hpp`

---

## Log Examples

```
[INFO] [strategy] [supertrend_scalper_ETH_USDT] SuperTrend flipped bullish 
       (price crossed above band) signal: confidence=0.4523, price=3478.90, atr=12.34
[INFO] [strategy] [supertrend_scalper_ETH_USDT] SuperTrend flipped bearish 
       (price crossed below band) signal: confidence=0.3876, price=3445.20, atr=11.89
[INFO] [strategy] [supertrend_scalper_ETH_USDT] Warming up: 8/11 candles accumulated 
       (need ~11 min of kline data)
```
