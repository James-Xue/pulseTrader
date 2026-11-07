# XAUUSD gold automated-trading sub-agent — strategy document

> Created 2026-08-17 · runs on the pulseTrader engine (mainnet live) · this file is the sub-agent's rule baseline; review it before modifying
> ⚡ 2026-08-19 scalping mode v3 deployed (user instruction: trade frequently, scalp): direction 0.2×ATR / RSI 40-60 / SL4-TP5-6 / time stop 20min / cooldown 1 bar / new trigger on momentum burst; the values in §3/§4 below are the historical v2 values, **the current rules are governed by the task document**
> ⚡ 2026-08-19 v3.1 upgrade package: silver-gold divergence (XAGUSD feed) + event-calendar gate g7 + news risk flag + session-aware weights. The current rules are governed by section v3.1 of the task document

## 1. Overview

Runs a resident background sub-agent on the pulseTrader engine that performs **conservative technical-signal scalping** on XAUUSD (Gate.io TradFi gold CFD):

- **0.01 lots** per trade (= 1 ounce ≈ 4,396 USD notional ≈ 8.7 USD margin @500x)
- at most **1 CFD position** at a time
- a cycle of about 25~30 seconds: fetch quotes → evaluate signals → open/close positions → risk checks
- writes a review markdown file to `0_note/` after every close

## 2. Market and Cost Background

| Item | Value |
|---|---|
| Trading direction | CFD gold (`active_market=cfd`, persists across restarts, does not fall back to futures) |
| Quotes | 1m klines (no volume field, volume=0) + ticker updated every second |
| Fees | Buy 0.06 USDT per 0.01 lots; no commission on sell; plus an overnight spread |
| Account | CFD 54.60 USD (2026-08-17 21:45 snapshot); realized +7.84 for the day (verification order +1.2 + TP order +6.64), round trips 2/4 |

## 3. Entry Signals (opens only when ≥2 conditions resonate)

Based on the most recent ≥40 1m klines (`get_market` klines=100):

1. **EMA crossover**: EMA9 / EMA21 point the same way (golden cross → long / death cross → short)
2. **RSI(14)**: oversold <30 → long / overbought >70 → short (or trend confirmation below/above 45)
3. **Support/resistance**: recent swing highs and lows; price **close to** support → long, close to resistance → short

**No-trade situations**: resonance of <2 conditions, counter-trend, sideways (EMA repeatedly gluing together and producing false crossovers), no clear direction.

## 4. Exit Rules

| Rule | Trigger | Action |
|---|---|---|
| Hard stop-loss | position unrealized_pnl ≤ **-5 USD** | close immediately |
| Take-profit | unrealized_pnl ≥ **+8 USD** | realize in the +8~+10 range |
| Trend reversal | reversal signal appears | close early |
| Daily-loss stop | cumulative realized loss for the day ≤ **-10 USD** | no new positions for the rest of the day (existing positions are still managed per the table above) |

## 5. Risk Control and Operating Protocol

- Every cycle confirm: `trading_halted=false`, no CFD position, rate-limit token available, balance sufficient for margin
- Open: `open_order` (XAUUSD, side, 0.01, market_type=cfd, type=market) — **must carry `sl_price`/`tp_price`** (M21 exchange-native attachments price_sl/price_tp, which take effect even if the engine crashes; verified live on 2026-08-17: TP 4413.11 was closed automatically by the exchange for +6.64 USD). Contract math: 0.01 lots = 1 ounce, a 1 USD/oz move = 1 USD PnL → long `SL=round(ask-5.0,2)`, `TP=round(ask+8.0,2)`; short `SL=round(bid+5.0,2)`, `TP=round(bid-8.0,2)` (since 2026-08-17 `get_positions` shows sl/tp immediately after the engine opens a position)
- Close: `close_position` (position_id from get_positions, closes the whole position by default)
- API error → skip this cycle, **never retry an order in an error state**
- Abnormal state (balance jump, position mismatch) → stop trading and record it
- Conservative first: **when unsure, do nothing**

## 6. Off-Limits

- ❌ BTC_USDT futures position (one existing short of 4 contracts) — watch only, do not touch
- ❌ user's manual trigger orders (sell@4418 / buy@4295 / sell@4428) — keep them
- ❌ `halt_trading` / `switch_direction` / `cancel_order` / strategy parameter changes

## 7. How It Runs

- Background sub-agent: runs ~10 rounds (~4-5 minutes) per turn, then reports; the main session automatically continues it → uninterrupted monitoring
- The user may at any time: feed in ideas/news (relayed by the main session), call a stop, adjust the rules
- Trade records: `0_note/gate-trading/gold/joey-Z170I-PRO-GAMING/review/xauusd-review-YYYYMMDD-HHMMSS.md`

## 8. Live Observations (first day, 2026-08-17)

- Gold ranged narrowly between 4389~4401; EMA9/21 produced repeated false crossovers (diff ±0.16, glued together), RSI 45~55
- Resonance conditions stayed ≤1/3 throughout → **0 opens, 0 closes**, stayed flat on standby (as the rules expected)
- To watch: entry quality when price is close to the 4388 support or the 4400 resistance and RSI moves out of its extremes

### Evening session addendum (2026-08-17 21:xx +08)

- **Market**: from 13:45Z gold rallied +23 USD in 15 minutes (4388→4411), RSI briefly reached 80 (deeply overbought); trading loop turn 1 (10 rounds) stayed HOLD throughout, correctly refusing to chase the long
- **Live verification of the engine's three fixes** (commit 36ee6f9): ① `get_positions` shows sl/tp immediately after opening; ② sync de-duplicates by exchange_position_id (no more repeated imports); ③ PnL is unleveraged (0.01 lots, 1:1 USD)
- **Native TP auto-realized**: test order 0.01 @ 4406.4, TP 4413.11 closed automatically by the exchange, realizing +6.64 USD; the position lasted 1.5 minutes and the engine did not intervene
- **Items to watch**: fill latency jitter (2.7s/13s, Clash proxy path); the signal board has had no new signals since 13:12Z (supertrend publishes only on flips); the +8h display bug is not fixed (the epoch is correct, only the ts_str label is wrong); the margin-basis discrepancy was resolved with the quanto fix (0.0877 was a product of the old sync hard-coding quanto=1.0; after the fix, 8.78 matches the basis documented in §1)
