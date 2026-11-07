# Backtest — wildcard backtest framework (M33)

Run 1m-kline backtests on **any Gate USDT-M perpetual symbol** locally, reusing the exact same
C++ strategy code path as live trading. Three tools:

| Tool | Purpose |
|---|---|
| `./run.sh backtest` | Single backtest: any symbol × any registered strategy × any parameters |
| `./run.sh kline-store` | Data accumulation: merge the trailing-window klines of any symbol into the local sqlite every day |
| `./run.sh sweep` | Parameter sweep: run through parameter combinations in one go, summarized to CSV |

## Data model (read this first)

The Gate REST futures 1m endpoint **only keeps the most recent ~10000 bars ≈ 6.9 days** (measured; earlier
requests all return 400 "too long ago"). Therefore:

- **Any symbol is testable immediately**: REST pulls the most recent ~7 days, no configuration needed.
- **The only source of a ~1-month window is accumulation**: `kline-store` pulls the trailing window once a day into
  `kline_bars` (idempotent `INSERT OR IGNORE`), and storage accumulates backwards. **A new symbol needs about 30 days
  to reach one month**; during that time a backtest = accumulated data + the REST tail.
- Downtime self-healing: missing a run does not matter; the next trailing-window pull fills it in automatically (**gaps ≤ ~6.9 days are seamless**);
  holes older than 7 days can never be filled (the exchange does not keep them) — you can only wait for future data.
- Symbols the engine has watched long-term (BTC/ETH/SNDK/UNITREE etc.) already have
  months of accumulation in `data/trades.db`; a single `kline-store --import` bootstraps them (see below).

## backtest — single run

```
./run.sh backtest --strategy momentum_scalper --symbol DOGE_USDT \
    --from 2026-09-01 --to 2026-09-06 --quantity 20 [--json run.json]
```

Registered strategies: 7, pick any with `--strategy`. All except `orderbook_scalper` consume klines
(orderbook_scalper only consumes the live order book, so it is expected to produce zero signals on replay; the engine warns).

`iron_trader` (added 2026-09-06) = the iron-rule trader: low-frequency, disciplined trend following; rule document
`docs/strategies/iron-trader.md`. Note that its **exit = Flat signal (close only, no reversal)**:
since 2026-09-06 BacktestAccount treats Flat as a close-only channel (the original semantics: a state signal, not a trade);
old strategies do not emit Flat and their behavior is unchanged; 1003→1016 tests all green.

### Key flags

| flag | Description |
|---|---|
| `--strategy NAME` | registration key: `momentum_scalper` / `mean_reversion_scalper` / `supertrend_scalper` / `eth_scalper` / `ema_resonance_scalper` / `orderbook_scalper` |
| `--symbol PAIR` | any USDT-M perpetual, e.g. `DOGE_USDT`, `PEPE_USDT`; unknown symbols produce a did-you-mean error (9306) |
| `--quanto Q` | contract multiplier (1 contract = Q base coins). **Resolved automatically by default**: public contract table → cached for 12h. Change the cache path with `--contracts-cache P`, disable with `--no-contract-cache` (when offline with no cache it explicitly errors and asks for `--quanto`; it never silently computes PnL with a wrong multiplier) |
| `--param KEY=VALUE` | **strategy parameter injection** (repeatable; CLI beats `--config`). Atomic hot-param keys: `order_quantity, min_confidence, ema_fast_period, ema_slow_period, bb_period, bb_std_dev, supertrend_period, supertrend_multiplier, cooldown_seconds, stop_loss_pct, take_profit_pct, auto_trade`; any other key goes through the custom_params channel (`eth_atr_step`, `eth_spike_filter_*`, `res_ema_p1..p5`, ...). The JSON report's `params.atomic / params.custom` record what actually ran |
| `--quantity Q` | position size (only scales PnL). `--param order_quantity=` only hot-updates the strategy-side atomic value and does not change the fill size |
| `--config PATH` | seed from a trading.toml instance: quantity/min_confidence (only when the CLI did not explicitly override) + **custom_params** (since M33) + **news_windows** (since M33.1, only when the CLI did not explicitly pass `--news`). Explicit CLI values win |
| `--news TIME[/X[/Y]]` | **major news-event gate window** (M33.1, repeatable, one per occurrence). T = event UTC time (ISO or epoch); X = `close_before_min` (default 15), Y = `resume_after_min` (default 15): no entries in [T−X, T+Y); a position crossing T−X is forced Flat (`exit_reason = news_blackout`). The same key `news_windows` exists at instance level in TOML; rules in docs/strategies/iron-trader.md §4.6. Example: `--news "2026-09-16T18:00:00Z/15/15"` (passed through by sweep) |
| `--from/--to TIME` | epoch seconds/milliseconds or ISO UTC; defaults resolve automatically from the local coverage |
| `--no-api` / `--no-cache` | use only local stock / do not write back to sqlite |
| `--db PATH` | kline_bars database (default `data/trades.db`) |
| `--json PATH` | full JSON report (data sources, signals, trades, equity curve, params) |
| `--fee-rate R` | <0 no fees; 0 market default (futures 0.0005 = 0.05% taker); >0 explicit |
| `--close-mode` | `flip` (default, an opposite signal closes the old and opens the new) / `independent` (positions coexist) |

### Inert parameters (injected but not consumed by the M33 fill model)

- `stop_loss_pct` / `take_profit_pct`: used by live risk control; the M33 backtest fills instantly at the kline close
  and has no SL/TP path (an intra-bar fidelity mode is a planned follow-up milestone).
- `ob_imbalance_threshold` / `ob_depth`: only consumed by orderbook_scalper and are inherently ineffective under kline
  replay.

### Semantics and limitations (an honest backtest boundary)

- Fill = instant fill at the signal kline's **close**; no slippage, no order book, no margin/liquidation/funding.
- Leverage is display-only and does not affect PnL; the return formula comes from the same source as the live `PositionManager`.
- Data quality: dirty candles in sqlite (>25% jumps etc.) are cleaned automatically; the report carries warnings.
- The local database auto-migrates to v2 on first open under an old schema (PK includes market_type), one-time, a few minutes.

## kline-store — data accumulation

```
./run.sh kline-store --symbols DOGE_USDT,PEPE_USDT          # pull the trailing window into storage
./run.sh kline-store --config trading.toml                  # [backtest] configuration
./run.sh kline-store --import data/trades.db --symbols BTC_USDT,SNDK_USDT  # import existing stock
```

- Cron twice a day (sleep-tolerant, idempotent):

```
0 7,23 * * * cd /home/joey/1_Code/09_pulseTrader && ./run.sh kline-store --config trading.toml >> logs/kline_store.log 2>&1
```

  (A systemd user timer alternative is in OPERATIONAL_GUIDE; public REST needs no API key.)
- `[backtest]` TOML section (optional):

```toml
[backtest]
store_db = "data/trades.db"
store_symbols = ["DOGE_USDT", "PEPE_USDT"]   # default = symbols of the enabled futures strategy instances in the config
contract_cache_path = "data/contracts_cache.json"
```

- Futures symbols are validated against the contract table first (typos are rejected instantly instead of wasting ~30 requests); spot can also be stored
  (`--market spot`); CFD is not supported (REST has no time-window endpoint; engine self-recording is the only source).
- `--import` copies futures rows from a kline_bars database of any schema version (optionally with
  `--symbols` filtering), filling in ~1 month for long-watched symbols in one go.

## sweep — parameter sweep

```
./run.sh sweep --strategy momentum_scalper --symbol DOGE_USDT \
    --from 2026-09-01 --to 2026-09-06 \
    '--param' 'ema_fast_period=5|9|21' '--param' 'min_confidence=0.5|0.7' \
    --quantity 20 --workers 4 --out results/sweep_doge.csv
```

- Value syntax: `5|9|21` discrete / `34..100:11` range / `2` fixed value, combined as a Cartesian product;
  keys are passed through via `--param` and classified by the engine into the atomic/custom channel, consistent with a single run.
- The remaining flags are passed through to backtest as-is (see the table above); `--max-combos` defaults to 500 to prevent blowups.
- The first run warms up serially and persists data; parallel runs all use the local data (the CSV `rows_api` column should be ≈0).
- Crash-safe: the CSV is rewritten after each completed row; rerunning the same command skips completed combinations by signature
  (`--force` reruns everything). Failed rows record `exit_code` + the stderr tail.
- **A sweep parameter set is an experiment artifact, not a registered strategy → it does not trigger the strategy-document rule**; if it proves useful
  and you want to trade it live, fix it as configuration/a new C++ strategy and add `docs/strategies/` documentation.

## Quick start (day one for a new symbol)

```bash
# 1. Accumulation starts today (no manual runs once cron is in place)
./run.sh kline-store --symbols DOGE_USDT

# 2. Test the most recent ~7 days immediately
./run.sh backtest --strategy momentum_scalper --symbol DOGE_USDT --quantity 20

# 3. Parameter experiment (parallel sweep after warming up local data)
./run.sh sweep --strategy momentum_scalper --symbol DOGE_USDT \
    --from 2026-09-01 --to 2026-09-06 --no-api \
    '--param' 'ema_fast_period=5|9|21' '--param' 'ema_slow_period=34..50:8'
```

## Milestone background

M29 single-strategy MVP (2026-08-23) → M33 wildcardization: A1 contract metadata auto-resolution
(QuantoResolver) + A2 full parameter injection → A3 kline-store data accumulation → B sweep.
kline_bars schema v2 (M0): the PK includes market_type, fixing the bug where spot/futures rows in the same minute
collided and were silently dropped (the VPS daily sync's futures rows had long been squeezed out by spot rows).
