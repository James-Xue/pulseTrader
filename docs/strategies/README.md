# pulseTrader Strategy Comparison Overview

> A comparison of the built-in scalping strategies: style, pros and cons, and suitable scenarios.

## Strategy Quick Reference

| Strategy | Type | Data source | Signal frequency | Warm-up time | Best market |
|------|------|--------|----------|----------|----------|
| **MomentumScalper** | Trend following | Kline close price | Low | ~21 kline bars | One-directional trend |
| **OrderBookScalper** | Microstructure | Order book depth | High | Instant | Ranging / high liquidity |
| **MeanReversionScalper** | Mean reversion | Kline close price | Medium | ~20 kline bars | Ranging range |
| **SuperTrendScalper** | Trend following | Kline H/L/C | Medium | ~11 kline bars | Trend / rising volatility |
| **EmaResonanceScalper** | Five-period resonance | Kline close price | Very low | ~201 kline bars | Strong one-directional trend |
| **GoldTrader** (L7, exit-only) | Dynamic stop ladder | Ticker + 1m klines | n/a (no entry) | ~period+1 bars for SuperTrend | XAUUSD CFD |

---

## Pros and Cons Comparison

### 1. MomentumScalper — EMA crossover momentum

**Pros**
- Simple logic, reliable signals (a crossover is an unambiguous trend turning point)
- Insensitive to noise; the EMA itself has a smoothing effect
- Good at capturing medium-length trending swings
- Few parameters (fast/slow period), easy to tune

**Cons**
- Noticeable lag: EMA is a lagging indicator, so the trend has already moved by the time the crossover occurs
- Poor performance in ranging markets: frequent false crossovers cause "sawtooth losses"
- Low signal frequency: you have to wait for a complete crossover cycle
- Cannot catch sharp reversals

**Style**: Classic trend following, suitable for daily-scale trends in major coins such as BTC/ETH.

---

### 2. OrderBookScalper — Order book imbalance

**Pros**
- Leading signals: order book pressure is a leading indicator of price movement
- High signal frequency: evaluated on every order book update
- No warm-up needed: works as long as depth data is available
- Suited to high-frequency trading: 100ms polling, low-latency response

**Cons**
- Many false signals: the order book can be pulled quickly (spoofing)
- Depth data is noisy: thresholds must be set sensibly to filter it
- Not suited to trending markets: it only sees micro pressure and ignores the macro direction
- Highly dependent on data quality: requires a stable WS connection

**Style**: Microstructure trading, suitable for short-term scalping in highly liquid markets (BTC/ETH futures).

---

### 3. MeanReversionScalper — Bollinger Band mean reversion

**Pros**
- Counter-trend trading: enters at overbought/oversold extremes
- Relatively high win rate: price is likely to revert to the mean
- Clear signals: touching a Bollinger Band is an unambiguous trigger condition
- Suited to ranging markets: most of the time the market is in a range

**Cons**
- Large losses in trending markets: in a one-directional trend price keeps breaking outside the bands
- Risk of adding to a losing counter-trend position: requires strict stop-loss discipline
- Medium signal frequency: must wait for price to touch the upper or lower band
- Parameter sensitive: the std_dev multiplier affects signal quality

**Style**: Mean reversion, suitable for bounce/pullback trading in sideways, ranging markets.

---

### 4. SuperTrendScalper — ATR trend flip

**Pros**
- Adaptive volatility: ATR automatically adjusts the band width to fit different market conditions
- Timely signals: triggers immediately when the trend flips
- Clear stop-loss: the SuperTrend line itself can serve as a dynamic stop-loss level
- Robust parameters: the multiplier works across a wide range

**Cons**
- Complex computation: needs H/L/C data and a multi-step calculation
- False signals in ranging markets: price repeatedly crosses back and forth around the band
- Slightly longer warm-up: needs period+1 kline bars
- Sensitive to gaps: a large gap can produce an abnormal ATR

**Style**: Volatility-adaptive trend following, suitable for markets with large volatility changes.

---

## Recommended Strategy Combinations

### Aggressive combination (high frequency)
- OrderBookScalper (primary) + MomentumScalper (secondary)
- Suitable for: highly liquid markets, seeking short-term profit
- Risk: high-frequency signal noise is large; strict risk control is required

### Conservative combination (low frequency)
- SuperTrendScalper (primary) + MeanReversionScalper (secondary)
- Suitable for: medium-volatility markets, seeking steady returns
- Risk: fewer signals; patience is required

### Balanced combination (current default)
- All 4 strategies enabled, aggregated by SignalAggregator (threshold 0.6)
- Suitable for: general market conditions
- Advantage: multi-strategy cross-validation reduces false signals

---

## Parameter Tuning Guide

| Strategy | Key parameter | Default | Tuning direction |
|------|----------|--------|----------|
| MomentumScalper | `ema_fast_period` | 9 | Shorten → more sensitive but more false signals |
| | `ema_slow_period` | 21 | Lengthen → smoother but more lag |
| OrderBookScalper | `ob_depth` | 5 | Increase → more stable but slower to respond |
| | `ob_imbalance_threshold` | 0.3 | Raise → fewer signals but more reliable |
| MeanReversionScalper | `bb_period` | 20 | Shorten → more sensitive |
| | `bb_std_dev` | 2.0 | Lower → more signals |
| SuperTrendScalper | `supertrend_period` | 10 | Shorten → faster response |
| | `supertrend_multiplier` | 3.0 | Lower → tighter bands |

---

## Signal Aggregation Mechanism

All strategy signals feed into `SignalAggregator`, which aggregates them as follows:

1. **Cooldown period**: each strategy cools down independently (30 seconds by default), preventing the same strategy from emitting signals back to back
2. **Threshold filtering**: only signals with confidence ≥ 0.6 are considered by the aggregator
3. **Multi-strategy validation**: when several strategies emit same-direction signals at the same time, the final signal confidence is higher
4. **Risk-control interception**: even with a strong signal, the risk layer may still reject execution due to position/drawdown limits

---

## Detailed Documentation

Detailed implementation documentation for each strategy:

- [MomentumScalper — EMA crossover momentum](momentum-scalper.md)
- [OrderBookScalper — Order book imbalance](orderbook-scalper.md)
- [MeanReversionScalper — Bollinger Band mean reversion](mean-reversion-scalper.md)
- [SuperTrendScalper — ATR trend flip](supertrend-scalper.md)
- [EthScalper — ETH short-chasing EMA crossover (coin-specific)](eth-scalper.md)
- [EmaResonanceScalper — five-period EMA resonance](ema-resonance-scalper.md)
- [GoldTrader — USD dynamic stop ladder for XAUUSD CFD (exit-only, L7)](gold-trader.md)
