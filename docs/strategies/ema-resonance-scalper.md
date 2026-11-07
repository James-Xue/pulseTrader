# EmaResonanceScalper — Five-Period EMA Resonance Strategy

> A trend-resonance strategy that emits a signal only when five EMAs (default 7/14/30/60/200) are strictly fully ordered.
> It enters only at the instant the resonance state transitions; it never chases and never re-emits.

## Core Principle

### Indicator definition

Five exponential moving averages (EMA), with default periods `7 / 14 / 30 / 60 / 200`:

| Period | Role | Sensitivity |
|--------|------|-------------|
| 7  | Fastest line | Most sensitive to price changes |
| 14 | Second-fastest line | Short-term rhythm |
| 30 | Middle line | Medium-term trend |
| 60 | Second-slowest line | Medium-term trend confirmation |
| 200 | Slowest line | Long-term trend baseline |

EMA recurrence (`computeEma`, k = 2/(period+1)): every new candle seeds with the SMA of the previous `period` closes in the snapshot and then recurses over the full series — **deterministic computation, no rolling-state drift**.

### Resonance definition (full ordering alignment)

| State | Condition | Meaning |
|-------|-----------|---------|
| **bull_aligned** | `EMA7 > EMA14 > EMA30 > EMA60 > EMA200` (strictly descending order) | Bullish resonance across all periods |
| **bear_aligned** | `EMA7 < EMA14 < EMA30 < EMA60 < EMA200` (strictly ascending order) | Bearish resonance across all periods |
| **mixed** | Anything else | No resonance, no signal |

All five lines stacked in the same direction means the trend has reached consensus on every timescale — the **strongest but rarest** signal.

### Signal logic (state-transition triggered)

Signals fire only at the instant the **resonance state changes**, not on every candle:

| State transition | Signal | Direction |
|------------------|--------|-----------|
| mixed → bull_aligned | Buy | Long |
| bear_aligned → bull_aligned (direct flip) | Buy | Long |
| mixed → bear_aligned | Sell | Short |
| bull_aligned → bear_aligned (direct flip) | Sell | Short |
| Same alignment persists | No signal | — |

- **No chasing**: no repeated signal while the alignment persists (the template cooldown is a second, independent gate)
- **No front-running**: no evaluation during warmup (default 201 candles)
- Every evaluation unconditionally submits the current resonance state (if the trend has changed, it has changed)

### Confidence calculation

```
confidence = clamp( |EMA7 − EMA200| / ATR14 × res_conf_scale, 0, 1 )
```

- Numerator = the **total span** of the resonance stack (fastest line to slowest line); the wider the stack, the stronger the trend consensus
- Denominator = ATR14 volatility normalisation: the same span scores high in a calm market and low in a violently choppy one
- `res_conf_scale` defaults to 1.0 and scales the whole value. The mainnet instance already uses 2.0: the first real signal measured
  |ema7-ema200|/ATR=0.5459, which multiplied by 1.0 failed the min_confidence=0.6 gate and was
  silently dropped by emitSignal (found in live verification on 2026-08-23; same family as eth_scalper's scale 20)

---

## Implementation Details

### Data requirements

- **Data source**: kline close prices (`onKline()` callback)
- **Minimum candles**: `klineNeeded = slowest period + 1` (default 201)
- **Warmup**: `warmupThreshold = 201` — SMA seeding for the slowest EMA needs 200 bars + the current one; KlineBuffer backfills 500 bars at engine startup, so real warmup takes only a few seconds

### Full EMA recomputation

Each evaluation calls `computeEma(closes, period, 0.0)` over every close in the snapshot:
- `prev = 0.0` triggers SMA seeding (mean of the first `period` closes in the snapshot) → full recursion
- Five independent, stateless lines → the result depends only on the kline series, with strong determinism
- 201 bars × 5 lines at 1-minute kline frequency is negligible overhead

### Resonance state machine

```
evaluateEntry:
  1. Read the five period params (custom_params, static)
  2. Extract closes → 5× computeEma full recomputation
  3. Strict full-ordering test → Bull / Bear / None
  4. Transition detection: m_hasPrev && res != prev && res != None
  5. Unconditionally commit m_prevResonance (commit first, then decide the signal — cooldown does not revoke an already committed state)
  6. Transition + ATR>0 → assemble Buy/Sell + confidence + indicators
  7. No transition → nullopt (state already committed)
```

### indicators snapshot (published with real signals)

```json
{ "ema7": 100.625, "ema14": 100.333, "ema30": 100.161,
  "ema60": 100.082, "ema200": 100.025,
  "resonance": "bull_aligned", "atr": 1.143 }
```

> The key names `ema7/14/30/60/200` follow the default periods; if periods are changed via custom_params, the values follow the `res_ema_p1..p5` configuration while the key names stay the same.

---

## Parameter Configuration

### custom_params (static, TOML instance-level, no hot reload)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `res_ema_p1` | 7 | Fastest EMA period |
| `res_ema_p2` | 14 | Second-fastest EMA period |
| `res_ema_p3` | 30 | Middle EMA period |
| `res_ema_p4` | 60 | Second-slowest EMA period |
| `res_ema_p5` | 200 | Slowest EMA period |
| `res_conf_scale` | 1.0 | Confidence scaling factor |

> Same channel as eth_scalper's per-coin parameters: static, read at construction. To hot-tune from MCP/AI, they must migrate to StrategyParams + the paramSetters/paramGetters whitelist (see the README architecture notes).

### trading.toml instance

```toml
[[strategy.instances]]
name            = "ema_resonance_scalper"
symbol          = "ETH_USDT"
market_type     = "futures"
leverage        = 10
margin_mode     = "cross"
order_quantity  = 20
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 500
custom_params = { res_ema_p1 = 7, res_ema_p2 = 14, res_ema_p3 = 30, res_ema_p4 = 60, res_ema_p5 = 200, res_conf_scale = 2.0 }
```

> **BTC_USDT instance of the same name (2026-09-04)**: the class is symbol-agnostic (the instance id
> becomes `ema_resonance_scalper_BTC_USDT` automatically), and every parameter is dimensionless (EMA periods +
> confidence scaling), so no per-coin rescaling is needed — just copy it as is:

```toml
[[strategy.instances]]
name            = "ema_resonance_scalper"
symbol          = "BTC_USDT"
market_type     = "futures"
leverage        = 10
margin_mode     = "cross"
order_quantity  = 1              # 1 contract = 0.0001 BTC (informational; signal_only)
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 500
custom_params = { res_ema_p1 = 7, res_ema_p2 = 14, res_ema_p3 = 30, res_ema_p4 = 60, res_ema_p5 = 200, res_conf_scale = 2.0 }
```

### Order mode (signal first, activate later)

The strategy code has full direct-order capability (Buy/Sell signals go through the normal aggregator path → risk gates). But a global engine switch decides whether orders are actually placed:

- With the current engine `signal_only = true` + `active_market = "cfd"` → this strategy **only publishes signals to the signal board**, and places no orders
- To activate auto-trading later: flip `signal_only = false` and `active_market = "futures"` (note: this simultaneously activates auto-ordering for all enabled strategies such as BTC/SNDK/UNITREE)

---

## Pros and Cons

### Pros

1. **Very high signal quality**: full five-EMA ordering is a strong trend consensus and naturally filters out false signals in choppy markets
2. **Clear state machine**: transition-triggered + no re-emit + cooldown as a second gate; behaviour is predictable
3. **Explainable confidence**: stack span / volatility directly reflects "consensus strength"
4. **Strong implementation determinism**: full recomputation with no rolling state, easy to test and replay
5. **Rules kept separate from code**: rule text (this document) ↔ C++ implementation (source) map one to one

### Cons

1. **Extremely rare signals**: full ordering is a demanding condition, triggered only by strong one-sided trends; a choppy market may go a long time without a signal
2. **Lag**: EMA is a lagging indicator, and the slowest 200-period line makes entries late, so entering near the end of a trend is risky
3. **No stop-loss/take-profit linkage**: the strategy only produces signals; risk management (position limits via the risk gates) and exits need external cooperation
4. **Sensitive to gaps**: a large gap opens the fast and slow lines instantly and may trigger alignment (the ATR normalisation in the confidence formula hedges part of this)

---

## Applicable Scenarios

### Best market conditions

- **Strong one-sided trends**: trend-following entries after a daily/4h-level trend is confirmed
- **Trend continuation**: full re-alignment after a deep pullback = a trend-continuation signal
- **Low-volatility advances**: confidence is high when cross-timescale consensus is clear and ATR is contracting

### Unsuitable markets

- **Wide ranges**: the five lines cross and tangle repeatedly, so full ordering is almost impossible (which is also why there are almost no false signals)
- **Violent spikes**: a sudden pump or pin bar can align the fast and slow lines momentarily; pair with spike filtering (see extension points)

---

## Working with Existing Strategies

| Combination | Usage |
|-------------|-------|
| With MomentumScalper (9/21) | The resonance strategy provides **trend confirmation** and the momentum strategy provides **fast-line timing**; same-direction signals from both carry higher confidence |
| With eth_scalper (short chasing) | Bearish resonance alignment can give the short-chasing grid a stricter trend gate (requires extending GridManager's readTrendGate input source) |
| With SuperTrendScalper | Both are trend-following and can cross-validate; resonance alignment + a same-direction SuperTrend flip = a strong signal |

---

## Extension Points (not implemented in v1)

1. **Per-candle state publishing**: eth_scalper v2 style, publishing a Flat state on every candle so the grid can use it as a trend gate — note that under production settings `emitSignal`'s min_confidence gate drops Flat signals with conf=0, so either set min_confidence=0 or add a Flat exemption to emitSignal
2. **Spike filtering**: pump/pin-bar filtering (can reuse eth_scalper's three-criteria filter pattern)
3. **Hot-tunable period parameters**: migrate into StrategyParams + the MCP/AI channel for online tuning

---

## Source Locations

- **Header**: `src/strategy/scalping/EmaResonanceScalper.hpp`
- **Implementation**: `src/strategy/scalping/EmaResonanceScalper.cpp`
- **Base class**: `src/strategy/scalping/UnifiedScalper.hpp` (template methods + computeEma/computeAtr)
- **Registration**: `src/strategy/StrategyRegistry.cpp` (`ema_resonance_scalper`)
- **Unit tests**: `tests/unit/strategy/test_ema_resonance_scalper.cpp`

---

## Log Examples

```
[INFO] [strategy] [ema_resonance_scalper_ETH_USDT] EMA resonance bullish alignment (all 5 EMAs ordered) signal: confidence=0.5251, price=102.50
[INFO] [strategy] [ema_resonance_scalper_ETH_USDT] EMA resonance bearish alignment (all 5 EMAs ordered) signal: confidence=0.5251, price=90.00
[INFO] [strategy] [ema_resonance_scalper_ETH_USDT] Warming up: 200/201 candles accumulated
```
