# FundingWatch — funding-rate window monitor (engine service, 2026-09-06)

> Pure observation service: **publishes to the signal board and logs only, never places an order, never connects to the aggregator** (it bypasses
> auto_trade and the order channel). Code `src/funding/FundingWatch.{hpp,cpp}`, registration section `[funding_watch]`.

## What Problem It Solves

The correct shape of funding-rate harvesting (spot long + futures short, collecting the "sentiment tax" paid by longs to shorts) is not
"collect rent every day", but **waiting for the rate window** (in the 2021 bull market BTC sustained 5-10bp/8h, while a calm market runs at
0.1-1bp). Verified over 7 years of full history (`data/funding/`, 2026-09-06):

| Year | BTC net return (0.5bp gate) | ETH net return | Market |
|---|---|---|---|
| 2020 | +16.3%/yr | +26.1% | Bull market |
| 2021 | +29.1% | +36.4% | Major bull market |
| 2022 | −1.6% | −4.3% | Bear market (still negative after the gate) |
| 2024 | +9.9% | +12.0% | Bull market |
| 2026 | ≈0 | ≈0 | Calm (current rate 0.1-1bp = no window) |

→ The value of this service = **automating "wait for the window"**: the engine stays silent before the window opens and lights up the instant
it opens. When auto-harvesting is wired up in the future, it only needs to consume the `funding_watch_<SYMBOL>` row on the signal board.

## Rules (the semantics are deliberately "dumb")

1. **Window OPEN** = the rates of the most recent `consec_events` settlements (default 6 ≈ 48h) are
   **all strictly greater than** `threshold` (default 0.0005 = 5bp/8h). The threshold is not ≥: exactly
   equal = the noise boundary, so it does not open.
2. **Window CLOSED** = any single settlement ≤ threshold. There is no hysteresis — the rate only settles
   once every 8h, so the window cannot flicker intraday.
3. Every poll (default 30 minutes; the rate moves once per 8h, which is enough) publishes the current state to the signal board:
   - `strategy_id = funding_watch_<SYMBOL>`
   - type **Sell** = window open (semantics: eligibility to harvest carry on the short side), **Flat** = closed
   - indicators: `funding_latest` / `funding_avg_24h` / `window_threshold` /
     `window_events`; the reason field carries a human-readable explanation
4. Engine log: **WARN only when the window flips** (once for OPEN plus once for CLOSED); polling is silent
   — if the window is open, it stays open; no log spam.

## Data Source and Failure Semantics

- Gate public endpoint `/futures/usdt/funding_rate` (8h settlement, retains ~180 days),
  reusing the main futures REST client and holding the shared `rest_mutex` for serialization (engine discipline).
- Fetch failure / empty response → silently skip this cycle, with the error log throttled (at most once every 5 minutes); **never
  report a window based on a network glitch**. Window determination looks only at consecutive events from successful polls.
- No dedicated thread: a main-loop tick slot (~200ms), with an internal slow gate limiting it to poll_sec.

## Configuration (same as `trading.toml.example`)

```toml
[funding_watch]
enabled = true
symbols = ["BTC_USDT", "ETH_USDT"]   # futures contracts
threshold = 0.0005                    # 5bp/8h (5-10bp is typical in a bull market)
consec_events = 6                     # ≈48h sustained
poll_sec = 1800                       # polling interval in seconds
```

Validation (when enabled): symbols non-empty, threshold ∈ (0, 0.01], consec ∈ [1,24],
poll_sec ∈ [60, 86400].

## Acceptance and Tests

- [x] Unit tests (10, pure functions, no network): window determination (empty / insufficient history / sustained high opens / a single gap
      breaks it / gaps outside the window ignored / strictly greater than the threshold / negative rates), Gate JSON parsing (ordering /
      truncation / malformed entries skipped) — all green (1042 total, 2026-09-06)
- [x] REST shape verified live (2026-09-06): `funding_rate?contract=BTC_USDT&limit=12`
      → `[{r: "0.000026", t: 1788681602}]` in reverse order, consistent with the parser
- [ ] Live acceptance (pending VPS deployment): after enabling, `get_signals` should show
      the `funding_watch_BTC_USDT` row plus the first poll log; in a calm market it should stay Flat
- [ ] Window captured live (pending a bull market): WARN OPEN when the rate stays >5bp for 48h consecutively; WARN CLOSED when it falls back
