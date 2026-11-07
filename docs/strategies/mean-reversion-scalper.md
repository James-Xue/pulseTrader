# MeanReversionScalper — Bollinger Band mean-reversion strategy

> Uses Bollinger Bands to identify overbought/oversold zones and trades against the move when price deviates from the mean.

## Core Principle

### Bollinger Bands

Bollinger Bands consist of three lines:

```
Middle band (SMA) = average close price over the past N kline bars
Upper band = SMA + k × standard deviation
Lower band = SMA - k × standard deviation
```

- **SMA**: simple moving average (the middle band)
- **Standard deviation**: measures how much price is fluctuating
- **k**: standard-deviation multiplier (2.0 by default)

### Signal logic

| Condition | Signal | Meaning |
|------|------|------|
| `price <= lower_band` | Buy | Price touches the lower band, oversold, a bounce is expected |
| `price >= upper_band` | Sell | Price touches the upper band, overbought, a pullback is expected |

### Confidence calculation

```
confidence = clamp(|price - band| / band_width, 0.0, 1.0)
```

- The further price deviates from the band, the higher the confidence
- `band_width = upper_band - lower_band`

---

## Implementation Details

### Data requirements

- **Data source**: kline close price (`onKline()` callback)
- **Minimum kline count**: `bb_period` (20 bars by default)
- **Polling interval**: 500ms

### Calculation steps

```cpp
// 1. Extract the close prices
std::vector<double> closes;
for (const auto& c : candles) {
    closes.push_back(c.close);
}

// 2. Compute the SMA
double sum = std::accumulate(closes.begin(), closes.end(), 0.0);
double sma = sum / closes.size();

// 3. Compute the standard deviation
double sq_sum = 0.0;
for (double price : closes) {
    double diff = price - sma;
    sq_sum += diff * diff;
}
double stddev = std::sqrt(sq_sum / closes.size());

// 4. Compute the Bollinger Bands
double upper_band = sma + bb_std_dev * stddev;
double lower_band = sma - bb_std_dev * stddev;
```

### Warm-up mechanism

```cpp
if (candles.size() < bb_period) {
    // Print warm-up progress once every 30 seconds
    PULSE_LOG_INFO("Warming up: {}/{} candles accumulated", 
        candles.size(), bb_period);
    return;
}
```

Warm-up time:
- 1-minute klines: ~20 minutes
- 5-minute klines: ~100 minutes

### Cooldown mechanism

```cpp
auto nowMs = current_time_ms();
if (nowMs - m_lastSignalTimeMs < cooldown_ms) {
    return; // do not emit a signal during the cooldown period
}
```

---

## Parameter Configuration

### Strategy parameters (StrategyParams)

| Parameter | Type | Default | Description |
|------|------|--------|------|
| `bb_period` | int | 20 | Bollinger Band period |
| `bb_std_dev` | double | 2.0 | Standard-deviation multiplier |
| `cooldown_seconds` | double | 30.0 | Signal cooldown time |

### trading.toml example

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

## Pros and Cons Analysis

### Pros

1. **Counter-trend trading**: enters at overbought/oversold extremes
   - Captures the profit from price reverting to the mean
   - Suited to ranging markets
2. **Relatively high win rate**: price is likely to revert to the mean
   - Statistically, 95% of prices lie within 2 standard deviations
   - Breaking outside the bands is a relatively rare event
3. **Clear signals**: touching a Bollinger Band is an unambiguous trigger condition
   - No complicated crossover judgement needed
   - Signals are easy to understand and verify
4. **Suited to ranging markets**: most of the time the market is in a range
   - Bollinger Bands work best when the market is sideways
   - Provides clear buy and sell points

### Cons

1. **Large losses in trending markets**: in a one-directional trend price keeps breaking outside the bands
   - "Walking the band"
   - Adding to a losing counter-trend position leads to huge losses
2. **Risk of adding to a losing counter-trend position**: requires strict stop-loss discipline
   - You cannot keep adding positions forever to average down the cost
   - A maximum-drawdown limit must be set
3. **Medium signal frequency**: must wait for price to touch the upper or lower band
   - Price does not break the bands every time
   - There may be no signal for a long period
4. **Parameter sensitive**: the std_dev multiplier affects signal quality
   - Multiplier too low → too many signals, many false ones
   - Multiplier too high → too few signals, missing opportunities

---

## Applicable Scenarios

### Best market conditions

- **Sideways range**: price oscillates within a range, where Bollinger Bands work best
- **Medium volatility**: too little volatility means few signals, too much means false signals
- **No clear trend**: avoid one-directional trending markets
- **Sufficient liquidity**: ensures continuous kline data

### Unsuitable markets

- **Strong trending markets**: price keeps moving in one direction and does not revert after breaking the bands
- **Extreme volatility**: Bollinger Bands fail during flash crashes or spikes
- **Low liquidity**: kline data is discontinuous and the standard-deviation calculation is distorted
- **Major events**: news-driven moves are not bound by technicals

---

## Tuning Recommendations

### Increase sensitivity (more signals)

```
bb_period = 15      # shorten the period
bb_std_dev = 1.5   # lower the standard-deviation multiplier
```

- More frequent signals
- Narrower bands, easier to break
- More false signals

### Increase stability (fewer signals)

```
bb_period = 30      # lengthen the period
bb_std_dev = 2.5   # raise the standard-deviation multiplier
```

- Fewer but more reliable signals
- Wider bands, breakouts are rarer
- Higher win rate

### Dynamic band width

Adjust the standard-deviation multiplier according to market volatility:

```cpp
double dynamic_std_dev = base_std_dev * (1 + volatility_factor);
// Widen the bands in high volatility, tighten them in low volatility
```

### Combine with trend filtering

Disable mean reversion in trending markets:

```cpp
if (adx > 25) {
    // Strong trend: disable the mean-reversion strategy
    return;
}
```

---

## Combining with Other Strategies

### Combining with MomentumScalper

- MomentumScalper determines the trend state
- In trending markets, MeanReversionScalper is disabled
- In ranging markets, MeanReversionScalper is enabled

### Combining with OrderBookScalper

- MeanReversionScalper identifies overbought/oversold zones
- OrderBookScalper confirms the direction of micro pressure
- Double confirmation of the reversal signal

### Combining with SuperTrendScalper

- SuperTrend determines the trend direction
- When the trend direction is opposite to the mean-reversion direction, the trend takes priority
- When the trend is unclear, mean reversion leads

---

## Source Locations

- **Header**: `src/strategy/scalping/MeanReversionScalper.hpp`
- **Implementation**: `src/strategy/scalping/MeanReversionScalper.cpp`
- **Parameter definitions**: `src/strategy/StrategyParams.hpp`

---

## Log Examples

```
[INFO] [strategy] [mean_reversion_scalper_ETH_USDT] Price at/below lower Bollinger Band 
       (oversold, mean reversion expected) signal: price=3420.15, upper=3480.50, 
       lower=3425.30, sma=3452.90
[INFO] [strategy] [mean_reversion_scalper_ETH_USDT] Price at/above upper Bollinger Band 
       (overbought, mean reversion expected) signal: price=3510.80, upper=3495.20, 
       lower=3440.60, sma=3467.90
```
