# Kline Display Bug Analysis — 2026-06-21

## Symptom

WebUI K-line chart shows no candle data despite the trading engine running for hours
with a successful WebSocket connection and active ticker/orderbook data flow.

## Root Cause Summary

| # | Severity | Component | Issue |
|---|----------|-----------|-------|
| 1 | **Critical** | `market_feed.cpp` | `on_kline_update()` cannot extract symbol from futures kline frames |
| 2 | **High** | `market_feed.cpp` | Kline subscription payload has reversed argument order |
| 3 | **Low** | `dashboard_state.cpp` | Chart only updates on new candle, not within-candle price changes |

---

## Bug #1 — Futures kline symbol extraction (Critical)

### Gate.io futures candlesticks frame format

```json
{
  "time": 1542162490,
  "channel": "futures.candlesticks",
  "event": "update",
  "result": {
    "t": 1542162480,
    "o": "6350.1",
    "c": "6350.2",
    "h": "6350.2",
    "l": "6350.1",
    "v": 120,
    "n": "BTC_USDT"
  }
}
```

Key observation: the contract name (`"n": "BTC_USDT"`) is **inside `result`**, not in the
outer frame. Compare with ticker and orderbook channels where the contract name IS in
the outer frame:

| Channel | Symbol location | Outer frame field |
|---------|----------------|-------------------|
| `futures.tickers` | `result["contract"]` | Not in outer frame |
| `futures.order_book` | `result["s"]` or `result["c"]` | Not in outer frame |
| `futures.candlesticks` | `result["n"]` | **Not in outer frame** |

Wait — the ticker parser extracts symbol from `result["contract"]`, and the orderbook
parser from `result["c"]`. Both work because they read from `result`, not `full_frame`.

But the kline parser reads symbol from `full_frame["contract"]`:

```cpp
// market_feed.cpp:313-322 (BEFORE FIX)
if (full_frame.contains("currency_pair"))
{
    symbol = full_frame["currency_pair"].get<std::string>();
}
else if (full_frame.contains("contract"))  // ← futures klines don't have this
{
    symbol = full_frame["contract"].get<std::string>();
}
if (symbol.empty()) { return; }  // ← always exits for futures klines
```

### Impact

100% of futures kline data is silently discarded. `KlineBuffer` stays empty.
`DashboardState::poll_klines()` never finds data. Frontend shows "Waiting for candle data...".

### Fix

Extract symbol from `result["n"]` for futures klines, matching how the field is
actually structured:

```cpp
// Spot: full_frame["currency_pair"] (e.g. "BTC_USDT")
// Futures: result["n"] (e.g. "BTC_USDT")
```

---

## Bug #2 — Kline subscription payload order (High)

### Gate.io API specification

Both spot and futures candlesticks require payload `[interval, contract]`:

```json
{
  "channel": "futures.candlesticks",
  "event": "subscribe",
  "payload": ["1m", "BTC_USDT"]
}
```

### Current code

```cpp
// market_feed.cpp:69-75 (BEFORE FIX)
std::vector<std::string> kline_payload;
for (const auto &symbol : symbols)
{
    kline_payload.push_back(symbol);   // "BTC_USDT" first
}
kline_payload.push_back("1m");         // "1m" second
// Result: ["BTC_USDT", "1m"]  ← WRONG ORDER
```

### Impact

Gate.io may silently reject the subscription or return no data. Even if the server
is tolerant, this is non-conformant and risks breakage.

### Fix

```cpp
std::vector<std::string> kline_payload;
kline_payload.push_back("1m");         // interval first
for (const auto &symbol : symbols)
{
    kline_payload.push_back(symbol);   // symbols after
}
// Result: ["1m", "BTC_USDT"]  ← CORRECT
```

---

## Bug #3 — Within-candle chart updates (Low)

### Current behavior

`DashboardState::poll_klines()` compares `latest_kline.open_time` with the stored
`last_kline_open_times_`. It only pushes a snapshot update when `open_time` changes
(i.e., when a new 1-minute candle forms). This means:

- The chart updates at most **once per 60 seconds**
- Current candle's OHLCV changes (which Gate.io pushes every ~2s) are never reflected
- The chart appears frozen between candle boundaries

### Fix

Also compare `latest_kline.close` (or `latest_kline.volume`) against the previously
cached value. When either changes, push an updated snapshot. This gives real-time
candle updates without excessive bandwidth (candle data is small).

---

## Files Modified

| File | Change |
|------|--------|
| `src/market/market_feed.cpp` | Fix symbol extraction in `on_kline_update()` + fix payload order |
| `src/webui/dashboard_state.cpp` | Add close-price change detection in `poll_klines()` |
| `src/webui/dashboard_state.hpp` | Add `last_kline_close_` map for within-candle tracking |
| `tests/unit/market/test_market_feed_stats.cpp` | Add kline parsing test for futures format |
| `docs/kline-bug-analysis-2026-06-21.md` | This analysis document |

## Verification

1. Run `./run.sh trade`, wait 60s → heartbeat should show non-zero `kline/s` rate
2. Open WebUI `http://127.0.0.1:8080` → K-line chart should show candles
3. Watch current candle update in real-time (not just every 60s)
4. `ctest --test-dir build --output-on-failure` → all tests green
