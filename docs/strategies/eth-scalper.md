# EthScalper — ETH short-chasing EMA crossover strategy (symbol-specific)

> ETH-only short-only strategy: an EMA bearish cross triggers Sell, combined with a
> three-measure spike filter and an ATR-adaptive take-profit hint. Since v2 it publishes a
> trend state on every kline (Flat + trend_state/spike) for the grid sub-agent to use as a
> trend gate — the first demo strategy showcasing UnifiedScalper's symbol-extension mechanism.

## Core principles

### Indicator definitions

Dual EMA (hot-adjustable, StrategyParams atomics):

| Period | Default | Role |
|------|------|------|
| `ema_fast_period` | 9 | fast line, crossover trigger |
| `ema_slow_period` | 21 | slow line, trend confirmation |

### Short-only semantics (short-chasing regime)

| Event | Signal |
|------|------|
| **bearish cross** (fast crosses below slow from ≥ slow) and not a spike | **Sell** (real signal) |
| bullish cross (fast crosses above slow) | no signal — **never chase longs** |

ETH is a short-chasing symbol: only short crosses are traded, and long crosses are always ignored.

### Spike filter (three measures, upgraded in the v2 post-mortem)

`range = high − low` (amplitude of a single 1m kline); if any of the three measures triggers, it is a spike:

| Measure | Default | Trigger condition |
|------|------|----------|
| `eth_spike_filter_usd` | 120 | range > 120 USD |
| `eth_spike_filter_pct` | 1.5 | range > 1.5% × close |
| `eth_spike_filter_atr` | 3.0 | range > 3.0 × ATR14 |

On a spike, **do not chase the short** (bearish cross + spike → emit only a Flat state, with reason "spike filter tripped").
Setting any measure to 0 disables that measure.

> Post-mortem lesson (eth-review-20260821-grid-v1.md): v1 had only the USD measure and could not stop the 04:50
> 1m +4.4% spike (the low-priced kline had a small USD amplitude, but the percentage/ATR measures catch it) — the three
> measures are the hardened defence against the accident where all 20 grid levels filled at the open.

### Trend-state publishing (v2, 2026-08-21)

Every kline **unconditionally** publishes one state entry:

- `type = flat`, `confidence = 0` (not a trading signal, it is a status channel)
- indicators always carry `trend_state` (bullish / bearish / neutral) + `spike` (0/1)

Purpose: the trend gate for the grid sub-agent/GridManager — **precondition for placing levels: new levels may only be placed when trend_state=bearish and spike=0**; consumers do not need to compute the EMA themselves.

> WARNING: live verification on 2026-08-23 found that the min_confidence gate in `emitSignal` drops Flat
> signals with conf=0 (the log prints before the gate, so the board never holds an entry) — a Flat exemption
> was added in `StrategyManager.cpp` (see git 4aacbe0); the aggregator already ignores Flat, so letting them through carries no trading risk.

### Confidence calculation

```
confidence = clamp( |ema_fast − ema_slow| / ATR14 × eth_min_confidence_scale, 0, 1 )
```

- Numerator = distance between the two lines; the wider the gap, the stronger the cross
- Denominator = ATR14 volatility normalization
- Mainnet instance uses `eth_min_confidence_scale = 20.0`: measured |EMA9-EMA21|/ATR ≈ 0.03;
  multiplying by 1.0 never clears the min_confidence=0.6 gate, so real signals are silently dropped (found live
  on 2026-08-23, the same family of problem as ema_resonance res_conf_scale=2.0); 20 brings a typical cross to ~0.65~1.0
  so it clears the gate while still honouring the 0.6 global semantics

### ATR-adaptive take-profit hint

```
suggested_tp = close − eth_atr_step × ATR14
```

The strategy only emits a **hint price** (indicators fields) and places no orders — high volatility gets a wide take-profit, low volatility a tight one.

---

## Implementation details

### Data requirements

- **Data source**: kline close prices (`onKline()` callback, 1m)
- **Minimum kline count**: `klineNeeded = max(slow_period + 1, 15)` (needed for slow-line SMA seeding + ATR14)
- **Warmup**: `warmupThreshold = slow_period` (21 bars by default)

### State machine

```
evaluateEntry:
  1. Read hot params (fast/slow period) + static symbol params (custom_params)
  2. Extract closes → dual EMA (prev seed = snapshot SMA, deterministic)
  3. trend_state: fast<slow=bearish / fast>slow=bullish / equal=neutral
  4. spike: three-measure range test, any trigger = spike
  5. bearish_cross: m_hasPrev && (prevFast>=prevSlow) && (fast<slow)
  6. Commit m_prevEmaFast/Slow unconditionally (a cross that happened has happened; cooldown does not undo it)
  7. bearish_cross && !spike && atr>0 → Sell + confidence
  8. otherwise → Flat state entry (trend_state + spike always carried)
```

### indicators snapshot (published with Flat/Sell)

```json
{ "ema_fast": 2417.78, "ema_slow": 2417.93,
  "trend_state": "bearish", "atr": 1.29, "atr_step": 0.05,
  "suggested_tp": 2417.82, "spike": 0,
  "spike_range_usd": 0.28, "spike_range_pct": 0.012,
  "spike_filter_usd": 120.0, "spike_filter_pct": 1.5, "spike_filter_atr": 3.0 }
```

---

## Parameter configuration

### Hot params (StrategyParams, adjustable via MCP/REPL)

| Parameter | Default | Description |
|------|------|------|
| `ema_fast_period` | 9 | fast period |
| `ema_slow_period` | 21 | slow period |
| `min_confidence` | 0.6 | emit gate (emitSignal); the mainnet instance keeps the 0.6 global semantics |
| `order_quantity` | 20 | contract count (seeded from TOML since 2026-08-23; it previously showed 0.001) |

### custom_params (static, TOML instance level, no hot reload)

| Parameter | Default value | Description |
|------|--------|------|
| `eth_atr_step` | 0.05 | take-profit hint distance multiplier (suggested_tp = close − step×ATR) |
| `eth_spike_filter_usd` | 120.0 | a single-kline amplitude above this USD value = spike |
| `eth_spike_filter_pct` | 1.5 | an amplitude above this percentage of close = spike |
| `eth_spike_filter_atr` | 3.0 | an amplitude above this ×ATR = spike |
| `eth_min_confidence_scale` | 1.0 | confidence scaling (mainnet 20.0, see above) |

### trading.toml instance

```toml
[[strategy.instances]]
name            = "eth_scalper"
symbol          = "ETH_USDT"
market_type     = "futures"
leverage        = 10
margin_mode     = "cross"
order_quantity  = 20
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 500
custom_params = { eth_spike_filter_usd = 120, eth_spike_filter_pct = 1.5, eth_spike_filter_atr = 3.0, eth_atr_step = 0.05, eth_min_confidence_scale = 20.0 }
```

> **BTC_USDT instance of the same name (2026-09-04)**: the class is symbol-agnostic (no ETH hard-coding; the instance
> id becomes `eth_scalper_BTC_USDT` automatically), so adding an instance is enough. **The USD measure must be re-based to the price scale**:
> `eth_spike_filter_usd=120` on ETH (~$2500) is ≈ 4.8% of an extreme single bar; copying it to $81k
> BTC equals 0.15%, so normal volatility would all be misread as a spike → the BTC version is relaxed to **2000 (≈2.5%)**;
> pct/atr/conf_scale are dimensionless measures and are reused. After the initial values go live, tune again against measured
> confidence/spike on the signal board (the same calibration process as ETH on 08-23). Example:

```toml
[[strategy.instances]]
name            = "eth_scalper"
symbol          = "BTC_USDT"
market_type     = "futures"
leverage        = 10
margin_mode     = "cross"
order_quantity  = 1              # 1 contract = 0.0001 BTC (informational, signal_only)
min_confidence  = 0.6
enabled         = true
poll_interval_ms = 500
custom_params = { eth_spike_filter_usd = 2000, eth_spike_filter_pct = 1.5, eth_spike_filter_atr = 3.0, eth_atr_step = 0.05, eth_min_confidence_scale = 20.0 }
```

### Order mode (signals first, activation later)

The engine-wide `signal_only = true` → this strategy only publishes signals to the signal board and places no orders. To activate auto-trading:
flip `signal_only` or run `autotrade ... on` for the instance (M28 one-switch). A restart returns to signals-only (fail-safe).

---

## Pros and cons analysis

### Pros

1. **Clear short-only discipline**: never goes long in the short-chasing regime; single, predictable semantics
2. **Status channel independent of trading**: Flat entries let consumers read the trend gate at any time without depending on cross events
3. **Three-measure spike filter**: USD/percentage/ATR complement each other and guard against wicks at any price scale
4. **Deterministic computation**: the prev seed is fully recomputed, with no rolling-state drift, which is easy for test replay
5. **Symbol-extension showcase**: fully demonstrates UnifiedScalper's three extension mechanisms (custom entry/state publishing/symbol params)

### Cons

1. **Lag**: EMA crossovers are inherently lagging; shorting near the end of a trend carries high risk
2. **No stop-loss/take-profit execution**: only emits a suggested_tp hint; protection must be implemented externally (risk gate/agent/grid)
3. **Repeated crosses in ranging markets**: crosses are frequent in range-bound conditions; there are no false signals, but there is plenty of status noise
4. **Confidence semantics need scaling**: the raw |EMA gap|/ATR magnitude is ~0.03, which does not match the global 0.6 gate
   and depends on the scale coefficient (mainnet 20.0) — when changing parameters you must understand this mapping layer

---

## Applicable scenarios

### Best market conditions

- **A clear downtrend**: the EMA stays in bearish alignment; a bearish cross after a pullback = a with-trend short entry
- **Precondition for grid short levels**: place/add levels when trend_state=bearish and spike=0 (ETH grid v2 protocol)

### Unsuitable markets

- **A strong one-way bull**: bullish crosses are ignored and the strategy is completely silent (the status is still published, direction = long)
- **Violent wick conditions**: the spike filter blocks most crosses (that is exactly the design intent)

---

## Interaction with existing strategies

| Combination | Usage |
|------|------|
| With ema_resonance_scalper | Short-side resonance alignment (bear_aligned) gives stronger trend confirmation for short-chasing; both bearish in the same direction = double confirmation |
| With GridManager (M27) | The state entries are the data source for the in-engine grid trend gate (readTrendGate); precondition for placing levels: trend_state=bearish && spike=0 |
| With momentum/moving-average strategies | Generic strategy signals + eth_scalper status can cross-validate the ETH direction consensus |

---

## Extension points (not implemented in v2)

1. **Long switch**: add a custom_params switch allowing a bullish cross to emit Buy (the short-chasing regime is currently hard-coded)
2. **Suggested entry price**: a suggested_entry similar to suggested_tp, giving agents/grids a fuller reference
3. **Hot-tunable symbol params**: after migrating custom_params into StrategyParams + the paramSetters whitelist, they can be tuned online via MCP/AI
4. **Multi-symbol reuse**: the parameter table is already generic; build same-named instances for other short-chasing symbols (one registration line in StrategyRegistry)

---

## Source locations

- **Header**: `src/strategy/scalping/EthScalper.hpp`
- **Implementation**: `src/strategy/scalping/EthScalper.cpp`
- **Base class**: `src/strategy/scalping/UnifiedScalper.hpp` (template method + computeEma/computeAtr)
- **Registration**: `src/strategy/StrategyRegistry.cpp` (`eth_scalper`)
- **Unit tests**: `tests/unit/strategy/test_eth_scalper.cpp`

---

## Log examples

```
[INFO] [strategy] [eth_scalper_ETH_USDT] ETH short setup: EMA bearish crossover (fast < slow) signal: confidence=1.0000, price=2412.56
[INFO] [strategy] [eth_scalper_ETH_USDT] ETH state: EMA trend bearish — no signal signal: confidence=0.0000, price=2412.67
[INFO] [strategy] [eth_scalper_ETH_USDT] ETH state: bearish crossover but spike filter tripped (range > usd/pct/atr threshold) — no chase
[INFO] [strategy] [eth_scalper_ETH_USDT] Warming up: 14/21 candles accumulated (need ~21 min of kline data)
```
