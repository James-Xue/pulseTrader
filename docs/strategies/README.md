# pulseTrader Strategy Comparison

> Style, strengths, weaknesses and suitable regimes for the four built-in scalping strategies.

## Strategy quick reference

| Strategy | Type | Data source | Signal frequency | Warm-up | Best regime |
|----------|------|-------------|------------------|---------|-------------|
| **MomentumScalper** | Trend following | Kline close price | Low | ~21 bars | Sustained trend |
| **OrderBookScalper** | Microstructure | Order-book depth | High | None | Range-bound / high liquidity |
| **MeanReversionScalper** | Mean reversion | Kline close price | Medium | ~20 bars | Range-bound |
| **SuperTrendScalper** | Trend following | Kline H/L/C | Medium | ~11 bars | Trending / rising volatility |

---

## Pros and cons

### 1. MomentumScalper — EMA crossover momentum

**Pros**
- Simple logic with reliable signals: a crossover is an unambiguous trend turning point
- Insensitive to noise, because the EMA already smooths the series
- Suited to capturing medium-length trend swings
- Few parameters (fast/slow period), so it is easy to tune

**Cons**
- Clearly lagging: the EMA is a lagging indicator, so the trend has already moved by the time the crossover prints
- Poor in choppy markets: frequent false crossovers produce a "sawtooth" of small losses
- Low signal frequency: it has to wait for a full crossover cycle
- Cannot catch a fast reversal

**Style**: classic trend following, suited to daily-scale trends on majors such as BTC/ETH.

---

### 2. OrderBookScalper — order-book imbalance

**Pros**
- Leading signal: order-book pressure moves ahead of price
- High signal frequency: it can evaluate every order-book update
- No warm-up: it works as soon as depth data exists
- Built for high frequency: 100 ms polling with a low-latency response

**Cons**
- Many false signals: orders can be pulled quickly (spoofing)
- Noisy depth data: thresholds have to be set sensibly to filter it
- Unsuitable in trending markets: it reads only micro pressure and ignores the macro direction
- Highly dependent on data quality: it needs a stable WS connection

**Style**: microstructure trading, suited to short-term scalping in highly liquid markets (BTC/ETH futures).

---

### 3. MeanReversionScalper — Bollinger-band mean reversion

**Pros**
- Counter-trend entries at overbought/oversold extremes
- Relatively high hit rate: price tends to revert to the mean
- Clear signals: touching a Bollinger band is an unambiguous trigger
- Suited to range-bound markets, where price spends most of its time

**Cons**
- Large losses in trending markets: in a sustained trend price keeps breaking outside the bands
- Averaging into a losing counter-trend position is a real risk; strict stops are mandatory
- Medium signal frequency: it waits for price to reach an outer band
- Parameter-sensitive: the `std_dev` multiplier drives signal quality

**Style**: mean reversion, suited to bounce/fallback trades in sideways markets.

---

### 4. SuperTrendScalper — ATR trend flip

**Pros**
- Volatility-adaptive: ATR adjusts the band width to the market regime
- Reasonably timely: it triggers as soon as the trend flips
- Clear stop level: the SuperTrend line itself can serve as a dynamic stop
- Robust parameters: the multiplier stays effective across a wide range

**Cons**
- More computation: it needs H/L/C data and a multi-step calculation
- False signals in chop: price crosses back and forth around the band
- Slightly longer warm-up: it needs period+1 bars
- Sensitive to gaps: a large gap can distort ATR

**Style**: volatility-adaptive trend following, suited to markets whose volatility changes sharply.

---

## Suggested combinations

### Aggressive (high frequency)
- OrderBookScalper (primary) + MomentumScalper (secondary)
- Suited to: highly liquid markets, targeting short-term profit
- Risk: high-frequency signal noise; strict risk control is required

### Steady (low frequency)
- SuperTrendScalper (primary) + MeanReversionScalper (secondary)
- Suited to: medium-volatility markets, targeting steadier returns
- Risk: fewer signals; patience is required

### Balanced (current default)
- All four strategies enabled, aggregated by `SignalAggregator` (threshold 0.6)
- Suited to: general market conditions
- Advantage: cross-validation between strategies reduces false signals

---

## Parameter tuning guide

| Strategy | Key parameter | Default | Tuning direction |
|----------|---------------|---------|------------------|
| MomentumScalper | `ema_fast_period` | 9 | Shorter → more sensitive, more false signals |
| | `ema_slow_period` | 21 | Longer → smoother, more lag |
| OrderBookScalper | `ob_depth` | 5 | Higher → steadier, slower to react |
| | `ob_imbalance_threshold` | 0.3 | Higher → fewer but more reliable signals |
| MeanReversionScalper | `bb_period` | 20 | Shorter → more sensitive |
| | `bb_std_dev` | 2.0 | Lower → more signals |
| SuperTrendScalper | `supertrend_period` | 10 | Shorter → faster response |
| | `supertrend_multiplier` | 3.0 | Lower → tighter band |

---

## Signal aggregation

Every strategy publishes into `SignalAggregator`, which combines them as follows:

1. **Cooldown** — each strategy cools down independently (30 s by default) so one strategy cannot fire continuously.
2. **Threshold filter** — only signals with confidence ≥ 0.6 are considered by the aggregator.
3. **Multi-strategy confirmation** — when several strategies emit the same direction at once, the resulting confidence is higher.
4. **Risk gate** — even a strong signal can still be rejected by the risk layer because of position or drawdown limits.

---

## Detailed documents

Implementation notes for each strategy:

- [MomentumScalper — EMA crossover momentum](momentum-scalper.md)
- [OrderBookScalper — order-book imbalance](orderbook-scalper.md)
- [MeanReversionScalper — Bollinger-band mean reversion](mean-reversion-scalper.md)
- [SuperTrendScalper — ATR trend flip](supertrend-scalper.md)
