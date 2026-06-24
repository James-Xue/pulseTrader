# WebUI K-line Chart Not Displaying — Root Cause Analysis and Fix Plan

> **Date**: 2026-06-20
> **Status**: ✅ **RESOLVED** (2026-06-23) — K-line panel replaced with TradingView Lightweight Charts v5 candlestick chart with Chart/Table toggle. Data pipeline bugs fixed in separate commits.
> **Frontend symptom**: K-line Chart panel shows "Waiting for candle data..."

## 1. Full Data Pipeline Trace

```
Gate.io Futures WS (fx-ws.gateio.ws)
  ↓ [futures.candlesticks channel subscription]
MarketFeed::on_kline_update()          ← Parse OHLCV JSON → push to KlineBuffer
  ↓
KlineBuffer (per-symbol, 500-entry ring buffer, seqlock)
  ↓
DashboardState::poll_klines()          ← Check for new K-lines every 200ms
  ↓
WsServer::push_snapshot()              ← JSON serialization → uWebSockets broadcast
  ↓
Frontend renderKline(snap.kline)       ← Renders OHLCV table (not graphical candlestick chart)
```

**Key code paths**:
- `src/market/market_feed.cpp:278-344` — `on_kline_update()` parses Gate.io JSON
- `src/market/kline_buffer.hpp` — Ring buffer (500 candles, seqlock)
- `src/webui/dashboard_state.cpp:229-262` — `poll_klines()` new K-line detection
- `src/webui/ws_server.cpp:57-77` — WebSocket broadcast
- `frontend/app.js:235-266` — `renderKline()` renders as HTML table

## 2. Log Evidence

### 2.1 WS Connection and Subscription (Normal)

```
[exchange.log 17:03:19-21]
WS proxy tunnel active: ... → wss://fx-ws.gateio.ws/v4/ws/usdt
WS connected to wss://api.gateio.ws/ws/v4/
WS subscribed to futures.tickers
WS subscribed to futures.order_book
WS subscribed to futures.candlesticks
```

WS successfully connected and remained connected for 13 minutes (17:03 → 17:15), network layer is functioning normally.

### 2.2 Market Data (Zero Events)

```
[market.log 17:03:19-21]
Starting futures MarketFeed for 1 symbols
Loaded 62 futures instruments from REST
futures MarketFeed started — subscribed to 1 symbols
[... No ticker/kline event logs within 13 minutes ...]
[market.log 17:15:59]
Stopping MarketFeed
```

### 2.3 Strategy Diagnostics (Completely Silent)

```
[strategy.log 17:03:21]
Started strategy: MomentumScalper on BTC_USDT
[MomentumScalper] Thread started, polling every 500ms
[... No "Waiting for kline data" or "Warming up" logs within 13 minutes ...]
[strategy.log 17:15:59]
[MomentumScalper] Thread exiting
```

**Conclusion**: The Gate.io server accepted the WS connection and subscription, but did not push any market data frames within 13 minutes.

## 3. Bugs Found

### Bug #1 (Critical): `poll_klines()` depends on ticker_cache as symbol index

**File**: `src/webui/dashboard_state.cpp:229-262`

```cpp
void DashboardState::poll_klines(DashboardSnapshot &snap)
{
    // ❌ Uses ticker_cache's symbol list to determine which kline buffer to query
    const auto symbols = market_feed_.ticker_cache().symbols();
    if (symbols.empty())
    {
        return;  // ← If ticker has no data, kline is completely abandoned, even if kline buffer has data
    }

    const auto &symbol = symbols[0];
    auto &kline_buf = market_feed_.get_kline_buffer(symbol);
    const auto latest_kline = kline_buf.latest();
    if (!latest_kline.has_value()) return;
    // ...
}
```

**Problem**: K-line and Ticker are independent WS channels. `poll_klines()` should not depend on ticker data to decide whether to check klines. Even if kline data has arrived, an empty ticker cache causes kline data to be completely ignored.

**Fix**: Iterate over `subscribed_symbols_` or `kline_buffers_` keys instead, without depending on `ticker_cache().symbols()`.

```cpp
// Fix suggestion: Use subscribed_symbols instead of ticker_cache().symbols()
void DashboardState::poll_klines(DashboardSnapshot &snap)
{
    // Use the subscribed symbols list provided by MarketFeed instead
    const auto symbols = market_feed_.subscribed_symbols();  // This method needs to be added
    // Or directly iterate over all keys in kline_buffers_
    for (const auto &symbol : symbols)
    {
        auto &kline_buf = market_feed_.get_kline_buffer(symbol);
        auto latest = kline_buf.latest();
        if (!latest.has_value()) continue;
        // ... Detect new K-line and update snap
    }
}
```

### Bug #2 (Medium): Strategy diagnostic logs depend on ticker arrival

**File**: `src/strategy/strategy_manager.cpp:196-201`

```cpp
// 1. Poll ticker for on_tick().
auto ticker_opt = feed->ticker_cache().get(cfg.symbol);
if (ticker_opt.has_value())      // ← Skips if ticker has no data
{
    strategy.on_tick(ticker_opt.value());
}
```

**Problem**: `on_tick()` contains the "Waiting for kline data" diagnostic logic, but it is only called after ticker data arrives. If no ticker arrives → `on_tick()` never executes → strategy diagnostics are completely silent → the user cannot determine the program state.

**Fix**: Trigger diagnostics even when ticker is empty (pass an empty ticker or add a new `on_no_data()` callback).

```cpp
// Fix suggestion: Notify strategy even when ticker is empty
auto ticker_opt = feed->ticker_cache().get(cfg.symbol);
if (ticker_opt.has_value())
{
    strategy.on_tick(ticker_opt.value());
}
else
{
    // Pass a default empty ticker so that on_tick()'s diagnostic branch executes
    strategy.on_tick(market::Ticker{});
}
```

### Bug #3 (Auxiliary): No INFO-level logs for WS data frames

**File**: `src/market/market_feed.cpp` — `on_ticker_update()`, `on_kline_update()`

**Problem**: These callbacks do **not produce INFO-level logs** when data is successfully received and processed. This makes it impossible to determine from runtime logs whether Gate.io is actually pushing data.

**Fix**: Add INFO-level logging for first data reception (log only once per symbol).

```cpp
// Fix suggestion: Add first-data log in on_kline_update()
void MarketFeed::on_kline_update(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    // ... Parse kline ...

    auto &buffer = get_kline_buffer(symbol);
    if (buffer.size() == 0)  // First K-line
    {
        PULSE_LOG_INFO("market", "First kline received for {} (open_time={}, close={:.2f})",
                       symbol, kline.open_time, kline.close);
    }
    buffer.push(kline);
}
```

### Bug #4 (Auxiliary): WS connection log uses wrong URL

**File**: `src/exchange/gate_ws_client.cpp:842`

```cpp
PULSE_LOG_INFO("exchange", "WS connected to {}", config_.wsUrl);  // ← Always prints spot URL
```

**Problem**: Regardless of whether the actual connection is to the spot or futures WS server, the log always prints `config_.wsUrl` (the spot URL). This is misleading during troubleshooting.

**Fix**: Print the actual `ws_url` variable in use.

```cpp
PULSE_LOG_INFO("exchange", "WS connected to {}", ws_url);
```

## 4. External Issues to Confirm

Even after fixing the above code bugs, we still need to confirm whether Gate.io Futures WS is actually pushing data normally:

| Check Item | Method |
|------------|--------|
| Whether Gate.io Futures WS actually sends no data | Re-run after fixing Bug #3, check market.log |
| Whether proxy interferes with futures WS | Temporarily disable proxy for direct connection test (if network permits) |
| Gate.io API changes | Manually connect to `wss://fx-ws.gateio.ws/v4/ws/usdt` with `wscat` and send subscription message to verify |
| Whether futures contract name "BTC_USDT" is correct | Check the name field returned by REST `/api/v4/futures/usdt/contracts` |

### Manual Verification Commands

```bash
# Test Gate.io futures WS with wscat (proxy required)
wscat -c "wss://fx-ws.gateio.ws/v4/ws/usdt" -x '{"time":1718900000,"channel":"futures.candlesticks","event":"subscribe","payload":["BTC_USDT","1m"]}'

# Or test REST endpoint with curl to confirm contract name
curl -s "https://api.gateio.ws/api/v4/futures/usdt/contracts" | jq '.[0].name'
```

## 5. Additional Finding: Frontend K-line Panel is a Table, Not a Chart

Currently, `renderKline()` in `frontend/app.js:235-266` renders K-line data as an **HTML table** (Time/O/H/L/C/V + red/green arrows), not a traditional candlestick chart.

**Future improvement** (non-blocking): Integrate [TradingView Lightweight Charts](https://github.com/nicehash/lightweight-charts) (~40KB gzipped) to implement proper candlestick chart rendering.

## 6. Fix Priority

| Priority | Bug | Impact |
|----------|-----|--------|
| 🔴 P0 | #1 poll_klines() depends on ticker_cache | K-line panel can never display (direct root cause) |
| 🟡 P1 | #2 Strategy diagnostics depend on ticker | User cannot determine whether the program is working correctly |
| 🟡 P1 | #3 No logs for WS data frames | Cannot determine from logs whether WS is receiving data |
| 🟢 P2 | #4 WS connection log URL is wrong | Misleading during troubleshooting |
