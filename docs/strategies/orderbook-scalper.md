# OrderBookScalper — Order book imbalance strategy

> Captures short-term directional price moves by analyzing the depth imbalance between bids and asks in the order book.

## Core Principle

### Order book imbalance indicator

**Imbalance** measures how unbalanced the bid/ask pressure is:

```
imbalance = (bid_volume - ask_volume) / (bid_volume + ask_volume)
```

- `imbalance > 0`: bid pressure exceeds ask pressure (bullish)
- `imbalance < 0`: ask pressure exceeds bid pressure (bearish)
- `imbalance ∈ [-1.0, 1.0]`: the normalized value

### Signal logic

| Condition | Signal | Meaning |
|------|------|------|
| `imbalance > threshold` | Buy | Bids are significantly stronger than asks; price may rise |
| `imbalance < -threshold` | Sell | Asks are significantly stronger than bids; price may fall |

### Confidence

```
confidence = clamp(|imbalance|, 0.0, 1.0)
```

The more severe the imbalance, the higher the signal confidence.

---

## Implementation Details

### Data requirements

- **Data source**: order book depth snapshot (`onOrderbook()` callback)
- **Depth levels**: by default the top 5 bid and ask levels are analyzed
- **Polling interval**: 100ms (the fastest strategy)

### Calculation steps

```cpp
// 1. Accumulate the bid volume across the top N levels
double bid_volume = 0;
for (int i = 0; i < depth; ++i) {
    bid_volume += book.bids[i].quantity;
}

// 2. Accumulate the ask volume across the top N levels
double ask_volume = 0;
for (int i = 0; i < depth; ++i) {
    ask_volume += book.asks[i].quantity;
}

// 3. Compute the imbalance
double imbalance = (bid_volume - ask_volume) / (bid_volume + ask_volume);
```

### Cooldown mechanism

Prevents signals in the same direction from clustering:

```cpp
auto nowMs = current_time_ms();
if (nowMs - m_lastSignalTimeMs < cooldown_ms) {
    return; // do not emit a signal during the cooldown period
}
```

The default cooldown is 30 seconds.

### Price selection

- **Buy signal**: uses `book.bids[0].price` (best bid)
- **Sell signal**: uses `book.asks[0].price` (best ask)

This ensures the signal price is the best price currently executable in the market.

---

## Parameter Configuration

### Strategy parameters (StrategyParams)

| Parameter | Type | Default | Description |
|------|------|--------|------|
| `ob_depth` | int | 5 | Number of order book depth levels analyzed |
| `ob_imbalance_threshold` | double | 0.3 | Imbalance threshold (0.0-1.0) |
| `cooldown_seconds` | double | 30.0 | Signal cooldown time |

### trading.toml example

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

## Pros and Cons Analysis

### Pros

1. **Leading signals**: order book pressure is a leading indicator of price movement
   - Large bids piling up → price is about to rise
   - Large asks piling up → price is about to fall
2. **High signal frequency**: evaluated on every order book update
   - Suited to high-frequency trading
   - Captures micro price movements
3. **No warm-up needed**: works as long as depth data is available
   - Can produce signals immediately after startup
   - Unlike kline strategies, no need to wait for data to accumulate
4. **Low latency**: 100ms polling, fast response

### Cons

1. **Many false signals**: the order book can be pulled quickly (spoofing)
   - Large orders can be bait and never actually fill
   - Needs volume confirmation
2. **Depth data is noisy**: the order book changes frequently
   - Thresholds must be set sensibly to filter it
   - Threshold too low → many false signals
   - Threshold too high → too few signals
3. **Not suited to trending markets**: it only sees micro pressure and ignores the macro direction
   - May trade counter-trend in a strong trend
   - Needs to be combined with trend indicators
4. **Highly dependent on data quality**: requires a stable WS connection
   - Cannot work when the WS connection drops
   - Depth data latency makes signals stale

---

## Applicable Scenarios

### Best market conditions

- **Highly liquid markets**: major coin futures such as BTC/ETH
- **Ranging markets**: price oscillates in a small range, where order book pressure is effective
- **Low volatility**: avoids depth data distortion caused by extreme moves
- **Stable WS connection**: ensures order book data is real-time and accurate

### Unsuitable markets

- **Low liquidity**: sparse depth data, unreliable signals
- **Strong trending markets**: order book pressure cannot predict trend continuation
- **Extreme volatility**: depth data is chaotic during flash crashes or spikes
- **Unstable WS**: frequent disconnects cause missing data

---

## Tuning Recommendations

### Increase sensitivity (more signals)

```
ob_depth = 3                    # look at only the top 3 levels
ob_imbalance_threshold = 0.2   # lower the threshold
cooldown_seconds = 15          # shorten the cooldown
```

- More frequent signals
- More false signals
- Suited to ultra-short-term scalping

### Increase stability (fewer signals)

```
ob_depth = 10                   # look at the top 10 levels
ob_imbalance_threshold = 0.5   # raise the threshold
cooldown_seconds = 60          # lengthen the cooldown
```

- Fewer but more reliable signals
- Slower response
- Suited to steady trading

### Depth weighting

Different levels carry different weights (nearer levels matter more):

```cpp
double weighted_bid = 0;
for (int i = 0; i < depth; ++i) {
    double weight = 1.0 / (i + 1); // level 1 weighs 1.0, level 2 weighs 0.5, ...
    weighted_bid += book.bids[i].quantity * weight;
}
```

---

## Combining with Other Strategies

### Combining with MomentumScalper

- MomentumScalper determines the direction of the larger trend
- OrderBookScalper trades only in the direction of the trend
- Filters out counter-trend signals and raises the win rate

### Combining with MeanReversionScalper

- MeanReversionScalper identifies overbought/oversold conditions
- OrderBookScalper confirms the direction of micro pressure
- Double confirmation of the reversal signal

### Combining with SuperTrendScalper

- SuperTrend determines the trend state
- Use MomentumScalper in a trend and OrderBookScalper in a range
- Requires market-state switching logic

---

## Source Locations

- **Header**: `src/strategy/scalping/OrderBookScalper.hpp`
- **Implementation**: `src/strategy/scalping/OrderBookScalper.cpp`
- **Parameter definitions**: `src/strategy/StrategyParams.hpp`

---

## Log Examples

```
[INFO] [strategy] [orderbook_scalper_ETH_USDT] Order book buy pressure 
       (imbalance > threshold) signal: imbalance=0.4523, bid_vol=12.3456, ask_vol=3.7890
[INFO] [strategy] [orderbook_scalper_ETH_USDT] Order book sell pressure 
       (imbalance < -threshold) signal: imbalance=-0.3876, bid_vol=5.1234, ask_vol=15.6789
```
