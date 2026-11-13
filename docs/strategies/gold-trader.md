# GoldTrader — USD dynamic stop ladder (XAUUSD CFD)

> Layer 7 exit supervisor · introduced 2026-11-13 · **does not open positions**

## 1. What it is

`GoldTrader` is the engine's "strategy 1" for Gate.io TradFi gold. Unlike the
scalpers in this folder, it has **no entry logic**: it supervises every open
`XAUUSD` CFD position — positions opened manually in the Gate app, positions
imported by `sync_positions`, and positions opened by any strategy — and
enforces a dynamic stop ladder in **account USD**.

It is loaded from the top-level `[gold_trader]` TOML section, not from
`[[strategy.instances]]`, so it starts with the engine and does not depend on
the signal-only/`auto_trade` gate (a protective exit is not a signal).

Implementation: `src/risk/GoldTrader.{hpp,cpp}` (L7), with the indicator in
`src/risk/SuperTrend.{hpp,cpp}`.

## 2. The rules

| # | Rule | Trigger | Action |
|---|------|---------|--------|
| 1 | Hard stop | unrealized PnL ≤ **-8 USD** | close the whole position at market |
| 2 | Break-even | peak floating profit > **+2 USD** | stop candidate = entry price |
| 3 | SuperTrend | 1-minute SuperTrend line ± **1 USD** buffer | stop candidate = line ∓ buffer; a trend flip against the position closes immediately |
| 4 | Peak give-back | peak floating profit sampled **every second**; armed once the peak exceeds **+2 USD**, then when current PnL ≤ peak − **5 USD** | close the whole position at market |
| 5 | Tighten-only | any pass | the effective stop is the tightest candidate ever seen; it never moves backwards |

All five candidates run on every pass. The effective stop is the **tightest**
one (highest price for a long, lowest for a short). The thresholds are all in
USD of unrealized PnL, not price points.

### Units

`0.01 lot XAUUSD = 1 ounce` (`quanto_multiplier = 100 oz/lot`). For the
minimum lot, a 1.00 price move equals 1.00 USD of PnL, so the defaults map to
8.00 / 2.00 / 5.00 / 1.00 price units. For a larger position the same USD
amount is divided by the ounce size:

```
usd_to_price = 1 / (quantity * quanto_multiplier)
pnl_usd      = (mark - entry) * quantity * quanto * side_sign
side_sign    = +1 long, -1 short
```

### Peak give-back semantics

`peakPnl` is derived on every pass from the best price seen and the CURRENT
entry/ounce size (`peakPrice - entry` scaled by ounces), not stored as USD:
the exchange re-averages `entry_price` when the user adds to the position, and
a frozen USD peak would fabricate a give-back level above the market. A
partial close changes the ounce size while the previously enforced stop price
is retained (tighten-only), which is deliberately more conservative than
recomputing a wider USD distance on the smaller remainder.

The give-back candidate is armed by `peak_trail_arm_usd` (default `+2 USD`,
the break-even trigger); before that the hard stop is the floor. This
preserves the explicit rule "loss -> 8U stop" instead of letting a +0.10 USD
tick tighten the stop to -4.90. Set `peak_trail_arm_usd = 0` for the literal
"peak - 5 USD always participates" reading. Above the trigger, break-even
(entry) dominates the give-back until the peak passes +5 USD; beyond that the
give-back locks in profit.

Example (long, entry 4000, 0.01 lot):

| Peak PnL | Break-even | Give-back stop | Effective stop |
|----------|------------|----------------|----------------|
| never positive | not armed | not armed | 3992.00 (-8) |
| +1.5 | not armed | not armed | 3992.00 (-8) |
| +3.0 | 4000.00 | 3998.00 | 4000.00 |
| +10.0 | 4000.00 | 4005.00 | 4005.00 |

### SuperTrend leg

- Timeframe: closed 1-minute candles only. The still-forming candle is
  excluded by the finality anchor below; the feed's `closed` flag alone is
  not trustworthy (it marks every candle `closed = true`, audit B6).
- Data path: the CFD feed polls klines on the wall-clock minute boundary and
  `KlineBuffer::updateOrPush` refreshes the newest bar in place, so each 1m
  bar converges to its final OHLC within ~1-2 s of closing instead of staying
  frozen at whatever partial snapshot the first poll saw. The SuperTrend leg
  additionally requires `MarketFeed::lastKlineFetchMs` to be at or after the
  bar's `close_time`, proving the bar was re-read after it closed; a bar seen
  only in its forming state is never used.
- Parameters: ATR period 10, multiplier 3.0 (same defaults as
  `SuperTrendScalper`).
- The engine uses the canonical Pine/TradingView flip rule: an uptrend ends
  when the close crosses below the previous **lower** band, a downtrend ends
  when it crosses above the previous **upper** band. (The Qt client's
  `IndicatorEngine` currently compares the opposite bands, which makes its
  direction alternate on every bar; the engine deliberately does not copy
  that behaviour.)
- A SuperTrend direction that opposes the position is treated as an immediate
  exit, not as a level. This is the "trend invalidated" case.

## 3. Exchange-side protection

Every tightened stop is mirrored to the exchange-native `price_sl` through
`EngineServices::modify_sl_tp` (PUT `/api/v4/tradfi/positions/{id}`):

1. The stop is rounded toward the safe side (floor for longs, ceil for
   shorts) at XAUUSD's two-decimal precision, so rounding never tightens a
   stop by accident.
2. A stop is only sent when it is tighter than what the exchange currently
   holds. A user's manually tightened stop in the Gate app is adopted as the
   new floor and never widened.
3. The exchange's current stop is reconciled on every pass: if it disappears
   or is loosened in the app, the next push restores the effective level
   (still only in the tightening direction).
4. A stop on the wrong side of the market is never sent; that case is a
   breach and closes at market instead.
5. Failed pushes are retried after `retry_cooldown_sec` (default 10 s);
   successful pushes do not start the cooldown, so consecutive tightenings
   reach the exchange immediately.

The modify request always rewrites both `price_sl` and `price_tp`, so when a
caller supplies only a stop the engine first reads the live position and
reuses the exchange's current take-profit. The engine's own copy can be up to
one 10-second sync behind; without the live read, a stop tightening could
revert (or clear) a take-profit the user just set in the Gate app.

While the gold trader is armed it is the **sole engine-side exit authority**
for its symbol: `ProtectiveExitManager` is given a filter and skips
`XAUUSD` CFD positions entirely, so the generic percentage trailing stop and
partial take-profit ladder cannot act on the same position (a 0.5% partial
take-profit would otherwise shrink the position and silently rescale the USD
thresholds).

This is what keeps the position protected if the engine process dies: the
exchange-side stop survives. The peak give-back rule is intentionally
engine-side only — it needs a per-second mark and closes at market.

The supervisor fails closed on symbol metadata: startup refuses to arm when
the CFD registry cannot prove `quanto_multiplier == contract_volume` (100
oz/lot for XAUUSD), and every pass re-checks the position's multiplier before
using the USD thresholds. Without that guard a failed registry load would
silently place stops 100 times too tight.

## 4. Timing and failure handling

- `tick()` runs on the engine main loop (5 Hz) and throttles itself to
  `poll_interval_ms` (default 1000 ms).
- A missing ticker quote skips the pass and keeps the current stops; a stale
  price never fabricates a trigger.
- A failed close is retried after `retry_cooldown_sec`; the supervisor never
  spams close requests.
- Per-position state (peak PnL, effective stop, last pushed stop) is dropped
  as soon as the position disappears, so a re-opened position starts with a
  fresh peak.

## 5. Configuration

```toml
[gold_trader]
enabled                = true
symbol                 = "XAUUSD"
market_type            = "cfd"
hard_stop_usd          = 8.0
breakeven_trigger_usd  = 2.0
supertrend_buffer_usd  = 1.0
peak_drawdown_usd      = 5.0
peak_trail_arm_usd     = 2.0
supertrend_period      = 10.0
supertrend_multiplier  = 3.0
contract_volume        = 100.0
poll_interval_ms       = 1000
retry_cooldown_sec     = 10
exchange_stop          = true
```

Validation (`config_validator.cpp`):

- `market_type` must be `cfd`.
- `hard_stop_usd`, `breakeven_trigger_usd`, `peak_drawdown_usd`,
  `supertrend_multiplier`, `contract_volume` must be > 0.
- `supertrend_period` must be in [1, 499] (the KlineBuffer capacity is 500),
  `supertrend_buffer_usd` ≥ 0, `peak_trail_arm_usd` ≥ 0.
- `poll_interval_ms` ≥ 100, `retry_cooldown_sec` > 0.

## 6. Observability

Startup log (`[L7] Gold trader armed: ...`) prints every configured
threshold. The control plane also exposes `get_gold_trader` (same name as the
MCP tool): it returns the armed/disabled state, the thresholds, and every
tracked position with mark, unrealized/peak PnL, effective stop and
exchange-side stop. The Qt client renders that payload as the **L7 dynamic
stop** card on the Gold Traders page; when the supervisor is disabled the
card shows the configured thresholds plus the reason.

Each stop tightening, push, failure, and close is logged on the `risk`
logger:

```
GoldTrader: XAUUSD_Buy_1 stop -> 4000.00 (breakeven, peak 3.00 USD, pnl 3.00)
GoldTrader: XAUUSD_Buy_1 exchange SL -> 4000.00 (tracked stop 4000.00)
GoldTrader: CLOSE XAUUSD_Buy_1 at 3999.90 (breakeven, pnl 0.10 USD, peak 3.00, stop 4000.00)
```

## 7. Tests

- `tests/unit/risk/test_gold_trader.cpp` — 25 cases: hard stop, break-even,
  peak give-back, SuperTrend buffer/flip, tighten-only, short mirroring,
  adopting and restoring exchange stops, sync-id reuse, add-to-position
  re-averaging, retry cooldowns, missing quotes, backward clock steps, poll
  throttling, contract-volume guard, disabled supervisor.
- `tests/unit/risk/test_protective_exit.cpp` — the gold-symbol exclusion
  filter keeps the percentage ladder off managed positions.
- `tests/unit/risk/test_supertrend.cpp` — rising/falling series, multiplier
  direction, warm-up and malformed input.
- `tests/unit/core/test_config_loader.cpp` and `test_config_validator.cpp` —
  parsing, defaults, and rejection rules.

## 8. Known limitations

1. The give-back is armed at the +2 USD peak trigger, not on any positive
   tick. With the defaults the effective ladder is: -8 USD until the peak
   passes +2, break-even (0) from +2 to +5, then peak − 5. Tune
   `peak_drawdown_usd` if a wider give-back is wanted.
2. Realized PnL and the recorded close price are engine estimates: the CFD
   close endpoint's body is not used for the fill price. The engine refreshes
   the mark immediately before closing to keep the estimate close.
3. Exchange-side `price_sl` updates are synchronous REST calls on the main
   loop, behind the shared REST mutex (a live positions read plus the PUT).
   A slow proxy can delay the 1 Hz cadence; the policy tolerates it (it only
   tightens).
4. The supervisor manages every XAUUSD CFD position, not a per-strategy book.
   If multiple engines ever ran against the same account, both would compete
   for the same stops (the `data/engine.lock` single-instance guard exists to
   prevent this).
5. Peak state is in-memory. After a restart the peak restarts at the PnL seen
   when the position is adopted, so a give-back derived from an earlier higher
   peak is not recovered. The exchange-native `price_sl` already attached is
   adopted as the new floor, so protection is never loosened — only the extra
   tightening from the lost peak is gone.
