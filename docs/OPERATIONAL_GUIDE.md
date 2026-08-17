# pulseTrader Operational Guide: From Zero to Live Trading

> This document is intended for operators, explaining how to advance pulseTrader from its current state to a production-ready trading system.
>
> Last updated: 2026-08-14 (M14 risk-gate hardening; product direction moved to CFD/TradFi gold)

---

## Table of Contents

1. [Current Status Assessment](#1-current-status-assessment)
2. [Missing Key Modules](#2-missing-key-modules)
3. [Development Roadmap](#3-development-roadmap)
4. [Operational Procedures (Assuming Main Program Ready)](#4-operational-procedures-assuming-main-program-ready)
5. [Key Parameter Tuning Guide](#5-key-parameter-tuning-guide)
6. [Risk Control System](#6-risk-control-system)
7. [Profitability Analysis](#7-profitability-analysis)
8. [Risk Warnings](#8-risk-warnings)
9. [FAQ](#9-faq)
10. [CFD (TradFi) — 黄金 CFD 实盘](#10-cfd-tradfi--黄金-cfd-实盘)

---

## 1. Current Status Assessment

### Completed 9-Layer Architecture

| Layer | Module | Responsibility | Status | Tests |
|-------|--------|----------------|--------|-------|
| L1 | Exchange | Gate.io REST + WebSocket API (spot + futures) | ✅ | 59 |
| L2 | Logging | spdlog async logging | ✅ | 8 |
| L3 | Market Data | Market data hot path (latency-sensitive, dual market) | ✅ | 38 |
| L4 | AI Analysis | Social/news → LLM → parameter tuning | ✅ | 43 |
| L5 | Heartbeat | 5-minute AI clock, TaskQueue | ✅ | 7 |
| L6 | Strategy | EMA crossover / order book imbalance / Bollinger mean reversion / SuperTrend ATR | ✅ | 66 |
| L7 | Risk Management | Position management / drawdown protection / rate limiting / stop-loss/take-profit / futures leverage risk control | ✅ | 104 |
| L8 | Execution | Order lifecycle management (dual market) | ✅ | 26 |
| L9 | Control Plane | JSON-RPC control socket (127.0.0.1:8081) + CLI REPL + stdio MCP server | ✅ | 65 |

**595 tests all passing** | `headless` branch (WebUI removed, replaced by the control plane) | Milestones M1–M14 all achieved

> **2026-08-14 update** — Risk-gate hardening (M14): single-evaluation order flow
> (fixed the 3002 reject loop + reservation leak), futures contract-multiplier
> notional (`quanto_multiplier` from the startup contract registry), and
> symmetric long/short fill tracking. Product direction: **CFD (TradFi) gold**
> — see [CFD_TRADFI.md](CFD_TRADFI.md) and §10 below.

### Currently Available Commands

```bash
./run.sh trade     # Start trading main program (9 layers chained, auto-loads trading.toml)
./run.sh trade --config trading.toml  # Start with specified TOML config file
./run.sh rest      # Test Gate.io REST connection (public + private endpoints)
./run.sh ws        # Test WebSocket real-time market data + private channels
./run.sh market    # Test market data pipeline (WS → L3 components)
./run.sh strategy  # Test strategy engine (simulated market data driving 4 strategies)
./run.sh ai        # Test AI Pipeline (--mock mode, no real LLM calls)
./run.sh cli       # Attach interactive REPL to a running engine (control socket)
./run.sh mcp       # Run stdio MCP server (bridges to the engine's control socket)
./run.sh test      # Run all 595 unit tests
```

### Trading Main Program (Completed)

`apps/pulsetrader/main.cpp` (~630 lines) chains all 9 layers into a complete trading system:

- **Construction order**: L2 Logger → L1 Exchange → L3 Market → L7 Risk → L8 Execution → L6 Strategy → L4 AI → L5 Heartbeat → L9 Control Plane
- **Signal flow**: StrategyManager → SignalAggregator → app callback (risk check → OrderFlowExecutor → OrderTracker). Manual orders opened via the control plane share the same `OrderFlowExecutor`
- **Order completion callback**: OrderTracker → PositionManager open/close + DrawdownGuard PnL update
- **Graceful shutdown**: SIGINT/SIGTERM → atomic stop flag → reverse-order shutdown (Control Plane → Strategy → Market → WS io_context::stop → ProxyTunnel poll+relay join → TradeRecorder → Logger)
- **Strategy factory**: `create_strategy()` creates concrete strategy classes based on configured names (MomentumScalper / OrderBookScalper / MeanReversionScalper)
- **Default config**: 2 strategies running on BTC_USDT, AI disabled, control socket on 127.0.0.1:8081, credentials read from `.env`

All existing commands are smoke test tools; `./run.sh trade` is the sole production-grade trade executor.

---

## 2. Missing Key Modules

### Must Implement (P0)

| Module | Description | Status |
|--------|-------------|--------|
| ~~Trade Recorder~~ | ~~SQLite persistence for every order~~ | ✅ Completed (Phase 2, M7) |
| **Futures Trading Support** | Gate.io USDT perpetual futures (dual market infrastructure + futures PnL/leverage/margin) | ✅ Completed (M10–M12) |
| — Config Foundation | MarketType/MarginMode enums, futures config fields, 7xxx error codes | ✅ Completed (Phase 3, M8) |
| — Exchange Layer Routing | EndpointRouter, WS ping/pong generalization, futures REST convenience methods | ✅ Completed (Phase 4, M9) |
| — Futures Market Data | Futures ticker/funding_rate/mark_price, SymbolInfo futures multiplier, dual MarketFeed | ✅ Completed (M10) |
| — Futures Risk Control | Leverage-aware PnL (qty×price×quanto×leverage), leverage/margin checks, liquidation price | ✅ Completed (M11) |
| — Futures Execution | Futures order format (contract/signed size), OrderTracker dual market, main.cpp chaining | ✅ Completed (M12) |

### Strongly Recommended (P1)

| Module | Description | Estimated Effort |
|--------|-------------|------------------|
| **Backtesting System** | Validate whether strategies are truly profitable using historical K-line data (currently no backtesting capability at all) | 1–2 days |
| **Paper Trading Mode** | Gate.io testnet or local simulated matching, validate end-to-end flow | 4–6h |
| **P&L Dashboard** | Profit/loss statistics view (daily/weekly/monthly P&L, win rate, profit/loss ratio) on the control plane | 3–4h |

### Nice to Have (P2)

| Module | Description | Estimated Effort |
|--------|-------------|------------------|
| **Telegram/WeChat Alerts** | Key event notifications (open/close position, stop-loss, drawdown protection triggered) | 2h |
| **Multi-Exchange Support** | Currently only Gate.io; extend to other exchanges | 1–2 weeks |
| **Hot Strategy Loading** | Add/remove strategies at runtime without restart | 4–6h |

---

## 3. Development Roadmap

```
✅ Phase 0: Trading Main Program                    ← Completed (apps/pulsetrader/main.cpp, 9 layers chained)
✅ Phase 1: TOML Config File Loading                ← Completed (config_loader + config_validator + trading.toml.example, 46 tests)
✅ Phase 2: SQLite Trade Recording                  ← Completed (trade_recorder, 17 fields, 4 query APIs, 27 tests, M7 achieved)
✅ Phase 3: Futures Config Foundation (M8)           ← Completed (MarketType/MarginMode enums, futures fields, 7xxx error codes, 18 tests)
✅ Phase 4: Futures Exchange Layer (M9)              ← Completed (EndpointRouter + WS ping/pong generalization, futures REST, 18 tests)
✅ Phase 5: Futures Market Data (M10)                ← Completed (Ticker/SymbolInfo futures fields, dual MarketFeed, 11 tests)
✅ Phase 6: Futures Risk Control & PnL (M11)         ← Completed (leverage-aware PnL, margin/leverage checks, liquidation price, 12 tests)
✅ Phase 7: Futures Execution & Dual Market Chaining (M12) ← Completed (futures orders, dual Executor/Tracker, main.cpp routing, 7 tests)
✅ Phase 8: Testnet Support (M13)                    ← Completed (PULSE_NETWORK switch, testnet REST + mainnet WS, 6 tests)
Phase 9: Testnet Paper Trading for 1 Week            ← In progress
Phase 10: P&L Analysis + Strategy Tuning             ← Estimated 2–3 days
Phase 11: Small Capital Live Trading (100 USDT)      ← Continuous observation
Phase 12: Gradual Position Sizing                    ← Data-driven decisions
```

### Phase 2 Detailed Tasks (Completed)

```
src/trade_recorder/ (new, completed):
  ├── trade_record.hpp — TradeRecord (17 fields) + TradeSummary POD structs
  ├── trade_recorder.hpp/cpp — RAII TradeRecorder, SQLite::Database, WAL + mutex
  ├── Table creation: trades (17 columns: id, order_id, client_order_id, timestamp_ns, symbol,
  │   side, order_type, requested_qty, filled_qty, avg_fill_price, submit_mid_price,
  │   slippage_bps, fees, pnl, latency_ms, final_status, strategy_name)
  ├── 4 query APIs: get_trades / get_trades_by_strategy / get_summary / get_daily_pnl
  ├── record_trade() — thread-safe INSERT (mutex-guarded, UNIQUE order_id)
  └── CMake: -DPULSE_ENABLE_SQLITE=ON to enable

apps/pulsetrader/main.cpp (modified):
  ├── #ifdef PULSE_ENABLE_SQLITE initializes TradeRecorder
  ├── OrderTracker completion callback calls recorder.record_trade()
  ├── sig.strategy_id passed through via client_order_id to trade_recorder
  └── Checkpoint + close on graceful shutdown

Tests (27, all passing):
  ├── test_trade_recorder.cpp — 15 core tests
  └── test_trade_queries.cpp — 12 query tests
```

### Phase 8 Detailed Tasks (Completed)

```
Phase 8: Testnet Support (M13):
  ├── config.hpp: ExchangeConfig added bool testnet field
  ├── config_loader.cpp: TOML [exchange] parses testnet field
  ├── config_validator.cpp: testnet + spot strategy → validation rejected (testnet is futures-only)
  ├── main.cpp: PULSE_NETWORK env var switches mainnet/testnet
  │   ├── testnet REST: https://api-testnet.gateapi.io
  │   ├── testnet WS: uses mainnet fx-ws.gateio.ws (testnet WS unreachable from China, market data is identical)
  │   ├── backward compatible: GATE_API_KEY/GATE_API_SECRET still work
  │   └── prominent log: ⚠️ TESTNET MODE — using virtual funds
  ├── .env structure: PULSE_NETWORK switch + mainnet/testnet key separation
  ├── trading.toml.example: testnet option documentation
  ├── run.sh: auto-loads trading.toml (no manual --config needed)
  ├── WebUI: fixed futures-only mode null pointer crash
  ├── SQLite: auto-creates data/ directory
  └── 6 new tests (3 loader + 3 validator), 503 all green
```

---

## 4. Operational Procedures (Assuming Main Program Ready)

### 4.1 Environment Setup

```bash
# 1. Create a Gate.io sub-account
#    - Purpose: isolate risk, one sub-account per strategy combination
#    - Up to 10 sub-accounts (VIP 0–4) or 30 (VIP 5–9)
#    - Sub-accounts inherit the main account's VIP tier
#    - ⚠️ Sub-accounts cannot be deleted once created

# 2. Fund the sub-account with starting capital
#    - Recommended to start with 100–500 USDT for testing
#    - Confirm the sub-account has sufficient USDT for trading

# 3. Create API Key
#    - ⚠️ Only enable "Spot Trading" permission, do NOT enable "Withdrawal" permission!
#    - ⚠️ IP whitelist: enter the server's public IP
#    - Record the API Key and Secret

# 4. Configure .env file
cat > .env << 'EOF'
# Network mode: "mainnet" (real money) or "testnet" (virtual funds)
PULSE_NETWORK=testnet

# Mainnet API Key
GATE_MAINNET_API_KEY=your_mainnet_key
GATE_MAINNET_API_SECRET=your_mainnet_secret

# Testnet API Key (https://fx-testnet.gateio.ws)
GATE_TESTNET_API_KEY=your_testnet_key
GATE_TESTNET_API_SECRET=your_testnet_secret

HTTPS_PROXY=http://127.0.0.1:7897
HTTP_PROXY=http://127.0.0.1:7897
PULSE_CONTROL_PORT=8081
EOF

# 5. Confirm .env is gitignored (already is)
grep ".env" .gitignore  # should produce output
```

### 4.2 Write Configuration File

```toml
# trading.toml — pulseTrader trading configuration
# Full template available at trading.toml.example

# Top-level keys must precede all [section]s
symbols = ["BTC_USDT", "ETH_USDT"]

[exchange]
apiKey = "from_env:GATE_API_KEY"
apiSecret = "from_env:GATE_API_SECRET"
restBaseUrl = "https://api.gateio.ws"
wsUrl = "wss://api.gateio.ws/ws/v4/"
proxyUrl = "from_env:HTTPS_PROXY"
restTimeoutMs = 10000
maxRetries = 3

[log]
level = "info"
logDir = "logs"
toConsole = true
toFile = true

# --- Strategy Configuration ---
[strategy]
signal_aggregator_threshold = 0.6   # Aggregated signal confidence ≥ 0.6 to execute (matches min_confidence for single strategy)
signal_cooldown_sec = 30             # Signal cooldown per symbol: 30 seconds

[[strategy.instances]]
name = "momentum_scalper"
symbol = "BTC_USDT"
order_quantity = 0.001               # 0.001 BTC per order (~$65)
min_confidence = 0.6
poll_interval_ms = 200               # 200ms market data polling interval

[[strategy.instances]]
name = "orderbook_scalper"
symbol = "BTC_USDT"
order_quantity = 0.001
min_confidence = 0.65
poll_interval_ms = 100               # Order book strategy needs more frequent polling

[[strategy.instances]]
name = "mean_reversion_scalper"
symbol = "ETH_USDT"
order_quantity = 0.01                # 0.01 ETH per order (~$35)
min_confidence = 0.6
poll_interval_ms = 500

# --- Risk Control Configuration ---
[risk]
maxPositionNotional = 500            # Fallback position cap 500 USDT (per market)
maxPositionNotionalFutures = 500     # Optional per-market override (futures)
maxPositionNotionalCfd = 500         # Optional per-market override (CFD)
maxPositionNotionalSpot = 500        # Optional per-market override (spot)
maxOpenPositions = 3                 # Max 3 simultaneous open positions (all markets)
maxDailyDrawdown = 0.02              # Daily loss ≥ 2% triggers halt
maxDrawdown = 0.05                   # Total drawdown ≥ 5% stops everything
maxOrdersPerSec = 5                  # Max 5 orders per second
maxSymbolNotional = 300              # Max position per symbol 300 USDT

[risk.stop_loss]
mode = "Trailing"                    # Trailing stop-loss
trailing_pct = 0.005                 # 0.5% trailing offset
max_hold_seconds = 300               # Max hold time 5 minutes

[risk.take_profit]
enabled = true
targets_pct = [0.005, 0.01, 0.02]   # 0.5% / 1% / 2% three-tier take-profit
fractions = [0.33, 0.33, 0.34]       # Close 33% / 33% / 34% at each tier

# --- AI Configuration ---
[ai]
backend = "openai"                   # or "claude"
model = "gpt-4o"
apiKey = "from_env:OPENAI_API_KEY"
heartbeatIntervalSec = 300           # AI analysis every 5 minutes
requestTimeoutMs = 30000

# --- Control Plane Configuration ---
[control]
enabled = true
bindAddress = "127.0.0.1"              # Localhost only — do NOT expose to the network
port = 8081                            # JSON-RPC control socket (env override: PULSE_CONTROL_PORT)
display_timezone = "local"             # Human-readable timestamps in control output:
                                       #   "local" (machine TZ) | "utc" | "±HH:MM"
                                       #   e.g. "-04:00" = US Eastern summer time —
                                       #   align with your phone app's timezone.
```

> **Control Socket Security Model**:
> - The control socket binds to **127.0.0.1 only** by default and has **no authentication** — anyone who can reach the port can place orders. Keep `bindAddress = "127.0.0.1"`; never bind to `0.0.0.0` or a public interface.
> - It speaks newline-delimited JSON-RPC 2.0 over TCP. Prefer the built-in `cli` REPL or the `mcp` server over raw socket access.

### 4.3 Start Trading

**Preferred: systemd user service (auto-start + crash restart + single instance)**

Install once (service file ships in `deploy/pulsetrader.service`):

```bash
cp deploy/pulsetrader.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now pulsetrader
loginctl enable-linger $USER          # start at boot without a login session
```

```bash
systemctl --user start pulsetrader        # start
systemctl --user status pulsetrader       # status
systemctl --user restart pulsetrader      # restart (e.g. after a rebuild)
journalctl --user -u pulsetrader -f       # live logs (journald, not engine.log)
```

- Enabled at boot via `loginctl enable-linger` — no manual start needed after reboot.
- The engine takes an exclusive `flock` on `data/engine.lock` at startup: a second
  engine process (manual `./run.sh trade`, another Claude session, etc.) is
  **refused immediately** — this prevents the 2026-08-16 double-engine incident
  (two engines each trading independently, exchange position ≠ engine view).
  Bypass with `PULSE_ALLOW_MULTI_INSTANCES=1` (not recommended).

**Startup position reconciliation**: the engine imports real open positions from
the exchange at startup (`GET /futures/usdt/positions`), so positions opened by a
previous run or manually appear in `positions` / `get_positions` immediately —
look for `[app] Position sync: ...` in the log. Synced positions count toward
risk limits (that is the point — true exposure). The position-notional cap is
enforced **per market type** (`maxPositionNotional{Futures,Cfd,Spot}` with
`maxPositionNotional` as fallback), so a large manual futures position
(e.g. a 5000-USDT SKHY short) blocks *futures* orders but not CFD orders;
`maxOpenPositions` and `maxSymbolNotional` remain global. Note: startup sync
covers futures only — CFD positions are not re-imported (CFD never trades
unless `switch cfd` is used).

**Alternative: manual launch (blocking, same binary)**

```bash
# Terminal 1: Start trading main program
./run.sh trade --config trading.toml

# Expected output:
# [INFO] pulseTrader v0.1.0 starting...
# [INFO] Exchange: Gate.io (REST + WS connected)
# [INFO] Market Data: subscribed to BTC_USDT, ETH_USDT
# [INFO] Strategies: 3 instances started
#   - momentum_scalper on BTC_USDT (200ms poll)
#   - orderbook_scalper on BTC_USDT (100ms poll)
#   - mean_reversion_scalper on ETH_USDT (500ms poll)
# [INFO] Risk Manager: max notional 500 USDT, daily DD limit 2%
# [INFO] AI Pipeline: heartbeat every 300s, next run in 5min
# [INFO] Control socket: 127.0.0.1:8081 (JSON-RPC, REPL + MCP)
# [INFO] Trading engine started. Press Ctrl+C to stop.
#
# Approximately 60 seconds after startup, the system begins printing a heartbeat log line every 60 seconds:
# [INFO] [heartbeat] uptime 1m00s | futures 100 tick/s  10 kline/s  80 ob/s | ws spot=n/a futures=connected | strategies 3/3 running | positions 0 (notional 0.00 USDT)
```

### 4.4 Monitor Operation

The WebUI was removed on the `headless` branch — monitoring and control now go through the **control plane**:

```bash
# Terminal 2: attach the remote REPL to the running engine (control socket)
./run.sh cli

# Or: if you started ./run.sh trade from an interactive terminal, an embedded
# REPL is already active there (stdin is a TTY) — type 'help' for commands.
```

**REPL command reference** (`help` inside the REPL):

| Command | Action |
|---|---|
| `status` | Engine status (uptime, feeds, halted) |
| `account` \| `balance` | Spot + futures balance |
| `positions` | Open positions + portfolio |
| `orders` | Active orders + recent reports |
| `strategies` | Registered strategies |
| `params <id>` | Strategy params |
| `set <id> <param> <value>` | Set strategy param (e.g. `set mom min_confidence 0.7`) |
| `open <sym> <buy\|sell> <qty> [--type market\|limit\|post_only] [--price P] [--market spot\|futures] [--leverage N] [--reduce-only] [--client-id S] [--sl P] [--tp P]` | Open an order; `--sl`/`--tp` attach exchange-native stop-loss/take-profit (**CFD only**) |
| `close <position_id> [qty] [price]` | Close a position |
| `cancel <order_id>` | Cancel an open order |
| `halt` / `resume` | Halt / resume all trading |
| `pause <id>` / `resume-strategy <id>` | Pause / resume a single strategy |
| `risk` | Risk snapshot (drawdown, rate limiter) |
| `market <sym> [--levels N] [--klines N] [--market spot\|futures]` | Market snapshot |
| `signals` | Signal board: latest per-strategy signals + indicators + aggregator consensus |
| `help` / `quit` / `exit` | Help / leave the REPL |

**Control-plane methods (= MCP tool names, 18 total)**: `get_status`, `get_account`, `get_positions`, `get_orders`, `list_strategies`, `get_strategy_params`, `set_strategy_param`, `open_order`, `close_position`, `cancel_order`, `halt_trading`, `resume_trading`, `get_risk`, `get_market`, `pause_strategy`, `resume_strategy`, `switch_direction`, `get_signals`. REPL commands map 1:1 to these methods over the control socket.

**Signal-only mode** — `[strategy] signal_only = true` makes strategies compute + publish signals to the signal board (`get_signals` / REPL `signals`) without ever placing orders; manual `open_order`/`close_position` remain live (the XAUUSD sub-agent's execution path). Board entries carry `ts_ms` + indicator snapshots — treat entries older than ~120 s as stale.

**MCP usage** — the `mcp` subcommand exposes the 18 methods as MCP tools over stdio for LLM clients (Claude Desktop / Claude Code):

```bash
claude mcp add pulsetrader -- /abs/path/build/apps/pulsetrader/pulsetrader mcp --config /abs/path/trading.toml
```

**MCP troubleshooting**:
- The trading engine must be running for `tools/call` to work (each call is forwarded over the control socket). `initialize` and `tools/list` work offline.
- Logs go to `logs/` as usual. In MCP mode stdout is reserved for the protocol stream, so console logging is forced off (`toConsole = false`) — diagnose MCP problems via `logs/*.log`, not stdout.
- `./run.sh cli` reports "cannot reach engine control socket" when the engine is down — start `./run.sh trade` first.

Or view logs directly:

```bash
tail -f logs/system.log      # System heartbeat (every 60s: market data rates, WS status, strategies, positions)
tail -f logs/strategy.log    # Strategy signals + warm-up progress
tail -f logs/exchange.log    # WS connection status
tail -f logs/app.log         # Order placement, risk control decisions
tail -f logs/risk.log        # Risk control events
tail -f logs/ai.log          # AI analysis results
```

> **⏱️ Strategy Warm-up Period**
>
> K-line-driven strategies (momentum_scalper, mean_reversion_scalper) need to accumulate 20–22
> 1-minute K-lines after startup before they begin working. During warm-up, `logs/strategy.log`
> reports progress every 30 seconds:
>
> ```
> [MomentumScalper] Warming up: 8/21 candles accumulated (need ~21 min of kline data)
> ```
>
> If WS is not connected, you will see:
> ```
> [MomentumScalper] Waiting for kline data (WS may not be connected yet)
> ```
>
> Please wait patiently for at least **25 minutes** to allow strategies to complete warm-up.

### 4.5 Stop Trading

```bash
# Graceful shutdown: Ctrl+C or send SIGTERM
# Main program stops each layer in reverse order:
#   1. L9: Stop control plane (control socket, REPL)
#   2. L6: Stop strategy engine (no new signals generated)
#   3. L3: Stop market data subscriptions (WS unsubscribes channels)
#   4. L1: Stop WS event loop (io_context::stop)
#          → Shut down ProxyTunnel (poll timeout exits accept thread,
#            close relay socket, join relay thread)
#   5. L8+: Close SQLite trade recorder
#   6. L2: Flush logs
#   7. Exit
#
# The entire shutdown process typically completes within 1 second
```

---

## 4.6 Maker-First Orders (order_type / maker_timeout_ms)

Strategy signals normally place market orders (taker fee — 0.05%/side on
futures, 0.1% round trip). With `order_type = "post_only"` or
`"maker_first"` on a strategy instance, signals instead place **post-only
limit orders at the exact best bid (buy) / best ask (sell)** from the live
order book (maker fee 0.02%/side — a 60% fee reduction):

| Config | Behavior |
|--------|----------|
| `order_type = "market"` (default) | Market order, taker fee — unchanged behavior |
| `order_type = "post_only"` | Maker order at best price; **never** crosses the spread. If no order-book data is available the signal is dropped |
| `order_type = "maker_first"` + `maker_timeout_ms = 500` | Post-only at best price; if unfilled after the timeout, the engine cancels and re-issues a market order for the **remaining** quantity (partial fills are topped up, never doubled) |

Key behavior:

- **Coverage**: strategy exits flow through the same signal path, so both
  open and close signals are maker-first. Manual `close_position` is
  unchanged (market, or limit if a price is passed).
- **No-chase policy**: if the exchange rejects a post-only order instantly
  (the price moved and the order would have crossed), the engine does NOT
  fall back to a taker order — the signal is dropped. Chasing a moved price
  costs a scalper more than a missed signal.
- **Book outage**: no book data → `maker_first` falls back to market
  immediately; `post_only` drops the signal. Never a stale-priced order.
- **Risk**: the fallback re-runs the full risk gate (rate limiter + notional
  reservation), so a drawdown halt or direction switch mid-attempt blocks it
  cleanly. `maker_timeout_ms` must be > 0 for `maker_first`.
- **CFD**: not supported — the TradFi API only accepts
  `price_type: market|trigger`. The config validator rejects any non-market
  `order_type` on `market_type = "cfd"`.

**Fee example**: 0.01 lot XAUUSD ≈ 4350 USDT notional — taker round trip
0.10% ≈ 4.35 USDT vs maker round trip 0.04% ≈ 1.74 USDT (saves ≈ 2.6 USDT
per round trip, if both legs fill as maker).

## 5. Key Parameter Tuning Guide

### 5.1 Strategy Parameters

| Parameter | Meaning | Tuning Direction |
|-----------|---------|------------------|
| `order_quantity` | Order size per trade | Start small (0.001 BTC), increase gradually after confirming profitability |
| `min_confidence` | Signal confidence threshold | Higher = more conservative (fewer trades but more precise); lower = more aggressive (more trades but more noise) |
| `poll_interval_ms` | Market data polling frequency | Lower = better latency but higher CPU usage. Recommended 100–500ms |
| `signal_aggregator_threshold` | Aggregated signal execution threshold | 0.6 = single strategy can trigger an order; with multi-strategy consensus, raise to 0.7+ |
| `signal_cooldown_sec` | Per-symbol signal cooldown | Prevents consecutive order placement. For scalping, recommended 15–60 seconds |
| `order_type` | `"market"` (default) / `"post_only"` / `"maker_first"` | Maker-first saves ~0.06% round-trip fees but risks missed fills in fast markets; raise `maker_timeout_ms` for liquid symbols, lower for volatile ones |
| `maker_timeout_ms` | Maker fill wait before taker fallback (ms) | 300–1000 typical for scalping; too long = stale signals enter late, too short = rarely fills as maker |

### 5.2 EMA Crossover Strategy (momentum_scalper)

```cpp
// Tunable parameters in strategy_params.hpp
ema_fast_period     = 9       // Fast line period (smaller = more responsive)
ema_slow_period     = 21      // Slow line period (larger = smoother)
ema_crossover_thresh = 0.001  // Crossover threshold (0.1%)
```

**Tuning recommendations**:
- Ranging market: increase `ema_slow_period` (e.g., 50) to reduce false signals
- Trending market: decrease `ema_fast_period` (e.g., 5) to capture trends faster

### 5.3 Order Book Imbalance Strategy (orderbook_scalper)

```cpp
ob_imbalance_window  = 5       // Depth levels
ob_imbalance_thresh  = 0.6     // Bid/ask ratio threshold (0.6 = bid volume accounts for 60%)
ob_refresh_ms        = 100     // Order book refresh interval
```

**Tuning recommendations**:
- High-volatility market: lower threshold to 0.55 for easier signal triggering
- Low-liquidity symbols: reduce depth levels to 3, focusing on near-book only

### 5.4 Bollinger Band Mean Reversion Strategy (mean_reversion_scalper)

```cpp
bb_period            = 20      // Bollinger Band period
bb_std_dev           = 2.0     // Standard deviation multiplier
bb_entry_thresh      = 0.001   // Entry threshold after touching band edge
```

**Tuning recommendations**:
- Suitable for ranging markets (when BTC is consolidating)
- Should be disabled during trending markets (will open positions against the trend)

### 5.5 AI Parameter Tuning

AI analyzes social/news sentiment every 5 minutes and outputs `ParamDeltas` to adjust strategy parameters:

```json
{
  "ema_fast_delta": -1,       // Speed up EMA fast line
  "ema_slow_delta": 0,
  "ob_thresh_delta": 0.05,    // Raise order book threshold
  "bb_std_delta": -0.2,       // Narrow Bollinger Bands
  "confidence_delta": 0.05,   // Raise confidence threshold
  ...
}
```

**Note**: AI parameter tuning effectiveness is highly dependent on prompt design. Initially, it is recommended to **disable AI parameter tuning** and first validate the base strategy's profitability.

---

## 6. Risk Control System

### 6.1 Multi-Layer Risk Control

```
Signal generation → Signal aggregation → Risk check → Order placement → Position monitoring → Stop-loss/Take-profit
                                                          ↓
                                                    Rejected if any condition is not met:
                                                    - Total position < maxPositionNotional
                                                    - Per-symbol < maxSymbolNotional
                                                    - Position count < maxOpenPositions
                                                    - Order rate < maxOrdersPerSec
                                                    - Daily loss < maxDailyDrawdown
                                                    - Total drawdown < maxDrawdown
```

### 6.2 Stop-Loss Strategies

| Mode | Description | Suitable Scenario |
|------|-------------|-------------------|
| **Fixed** | Fixed stop-loss at entry price ±1% | Simple and straightforward, suitable for beginners |
| **Trailing** | Tracks best price, triggers on 0.5% drawdown | **Recommended**, suitable for trending markets |
| **TimeBased** | Force-close position after 5 minutes | Ultra-short-term scalping |

### 6.3 Take-Profit Ladder

```
Entry price → +0.5% close 33% → +1.0% close 33% → +2.0% close 34%
```

Tiered take-profit allows you to:
- Lock in partial profits, avoiding profit giveback
- Let remaining positions benefit from larger price moves
- Reduce risk per individual decision

### 6.4 Circuit Breaker Mechanism

| Condition | Action |
|-----------|--------|
| Daily loss ≥ 2% | Stop opening new positions; existing positions continue to be managed |
| Total drawdown ≥ 5% | Close all positions, system halts, manual restart required |
| Order rate exceeds limit | Discard excess signals, log warning |

### 6.5 Operational Security

- ✅ API Key only has trading permission, **NOT withdrawal permission**
- ✅ Use sub-accounts to isolate risk
- ✅ IP whitelist restricts API access
- ✅ `.env` file is gitignored
- ✅ Control socket binds to **127.0.0.1 only** (no auth — never expose to the network)
- ⚠️ Current configuration uses **mainnet** (not testnet) — real money

---

## 7. Profitability Analysis

### 7.1 Fees Are the Biggest Enemy

| Item | Rate | Description |
|------|------|-------------|
| Gate.io Spot Taker | 0.2% | Taker side (market order) |
| Gate.io Spot Maker | 0.2% | Maker side (limit order) |
| Round trip | **0.4%** | Buy + sell |
| VIP 1 (≥1M/month) | 0.15% | Round trip 0.3% |
| Point card payment | 20% discount | Pay fees with GT tokens |

### 7.2 Break-Even Calculation

```
Assumptions:
  - Each trade: 0.001 BTC ≈ $65
  - Fees round trip: 0.4% = $0.26
  - Estimated slippage: 0.1% = $0.065

Break-even:
  - Each trade profit must be > $0.325 (0.5%) to cover costs
  - If win rate is 55%, profit/loss ratio needs to be > 0.82:1
  - If win rate is 50%, profit/loss ratio needs to be > 1.0:1 (i.e., avg win = avg loss)

Conclusion:
  - Scalping profit margin is extremely narrow (0.5%–2%)
  - Fees + slippage consume 20%–60% of profits
  - Need win rate > 55% or profit/loss ratio > 1.5:1 for consistent profitability
```

### 7.3 Impact of Latency

```
Your setup (China + proxy):
  - Network latency: ~100–200ms (to Gate.io servers)
  - Proxy additional latency: ~20–50ms
  - Total round-trip latency: ~250–500ms

Real HFT:
  - Co-location: ~1ms
  - Same data center: ~0.1ms

Conclusion:
  - You cannot be the fastest; do not compete on speed with institutions
  - Suitable for 1–5 minute mid-frequency strategies
  - Avoid second-level scalping (will be eaten by faster counterparties)
```

### 7.4 Reasonable Profit Expectations

| Scenario | Monthly Return | Conditions |
|----------|----------------|------------|
| Conservative | 2–5% | Low frequency, strict risk control, ranging market |
| Moderate | 5–10% | Mid frequency, effective strategies, favorable market |
| Aggressive | 10–20% | High frequency, large positions, higher risk |
| Loss | -5% ~ -100% | Ineffective strategies, black swan events, risk control failure |

**Reality**: Most individual quantitative traders end up losing money. Institutions have speed advantages, data advantages, and capital advantages. The core competitive edge for individual quant traders lies in: flexibility (ability to quickly switch strategies) and zero management fees.

---

## 8. Risk Warnings

### 8.1 Technical Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| Network disconnection | Unable to close positions | Stop-loss orders + manual close fallback channel |
| Proxy failure | Market data delay | Health checks + automatic reconnection |
| Program bugs | Incorrect order placement | Risk control layer interception + small capital trial runs |
| API changes | Interface failures | Version pinning + error handling |
| Server downtime | Unattended operation | Cloud provider + monitoring alerts |

### 8.2 Market Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| Flash crash | Instant large losses | Daily loss circuit breaker (2%) |
| Liquidity drought | Huge slippage | Position limits (300 USDT per symbol) |
| Exchange malfunction | Unable to trade | Diversify across multiple exchanges |
| Strategy failure | Consecutive losses | Drawdown circuit breaker (5%) |

### 8.3 Operational Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| API Key leakage | Asset theft | No withdrawal permission + IP whitelist |
| Misoperation | Unintended order placement | Testnet first + confirmation procedures |
| Configuration errors | Abnormal parameters | Config file validation + reasonable defaults |

---

## 9. FAQ

### Q1: How to use testnet?

Already supported (M13). Setup:

```bash
# Set in .env
PULSE_NETWORK=testnet

# Set in trading.toml
[exchange]
testnet = true
apiKey = "from_env:GATE_TESTNET_API_KEY"
apiSecret = "from_env:GATE_TESTNET_API_SECRET"
```

Notes:
- Testnet REST endpoint: `https://api-testnet.gateapi.io` (virtual funds, code auto-configures based on `testnet=true`)
- Market data WS uses mainnet (testnet WS is unreachable from China; market data is identical between mainnet/testnet)
- Testnet only supports futures, not spot — strategies must set `market_type = "futures"`
- Testnet API Keys must be created separately at https://fx-testnet.gateio.ws
- A prominent `⚠️ TESTNET MODE` notice is displayed at startup

### Q2: How many strategies can run simultaneously?

The current architecture supports multiple strategies running in parallel (one `std::jthread` per strategy), practically limited by:
- CPU core count (one thread per strategy + market data thread)
- Risk control limits (`maxOpenPositions = 5`)
- Recommendation: start with 2–3 strategies, observe results before adding more

### Q3: Is AI parameter tuning actually useful?

**Uncertain**. This is an experimental feature:
- LLM analyzes social/news sentiment → outputs parameter adjustment suggestions
- Effectiveness is entirely dependent on prompt design and market conditions
- Recommended to **disable AI parameter tuning** initially (`heartbeatIntervalSec = 0`), validate the base strategy first
- After confirming base strategy profitability, enable AI and observe results

### Q4: Why choose Gate.io?

- REST + WebSocket API documentation is comprehensive
- Supports sub-accounts (risk isolation)
- Fees are relatively reasonable (0.2%)
- Liquidity is acceptable (BTC/ETH major pairs)
- Drawback: latency is not as good as Binance; requires a proxy from China

### Q5: How to determine if a strategy is effective?

After running for 1 week, check statistics:

| Metric | Passing Grade | Excellent Grade |
|--------|---------------|-----------------|
| Win rate | > 50% | > 60% |
| Profit/loss ratio | > 1.0 | > 1.5 |
| Sharpe ratio | > 1.0 | > 2.0 |
| Max drawdown | < 10% | < 5% |
| Daily trade count | 5–20 | 10–30 |
| Net profit (after fees) | > 0 | Monthly > 5% |

### Q6: How to troubleshoot issues?

```bash
# 1. Check connectivity
./run.sh rest    # Does REST work?
./run.sh ws      # Is WS receiving real-time market data?

# 2. Check market data
./run.sh market  # Are L3 components updating normally?

# 3. Check strategies
./run.sh strategy  # Are strategies generating signals?

# 4. Check AI
./run.sh ai      # Is AI returning analysis results normally?

# 5. Check logs
ls logs/
cat logs/exchange.log   # Connection errors?
cat logs/strategy.log   # Abnormal signals?
cat logs/risk.log       # Risk control triggered?
cat logs/execution.log  # Order placement failures?

# 6. Control plane
./run.sh cli    # Attach REPL — status / positions / risk / market to view real-time state
```

### Q7: No orders placed after startup?

Check the following checklist item by item:

1. **Is the system alive?** — `tail -f logs/system.log`
   - Seeing `[heartbeat] uptime ...` every 60 seconds → system is running normally, just hasn't triggered a trading signal yet
   - If no output at all after 60 seconds → process may be hung, check `logs/exchange.log` to troubleshoot WS connection
2. **Is WS connected?** — `grep "WS connected" logs/exchange.log`
   - If you see repeated `WS connection failed` → check if proxy (`HTTPS_PROXY`) is working
3. **Are strategies warming up?** — `tail -f logs/strategy.log`
   - Seeing `Warming up: X/N candles` → normal, need to wait ~22 minutes to accumulate K-line data
   - Seeing `Waiting for kline data` → WS not connected, no market data flowing in
4. **Is the aggregator threshold too high?** — For single strategy, `signal_aggregator_threshold` should be ≤ the strategy's `min_confidence`
   - Default momentum_scalper has min_confidence=0.6, threshold should be set to 0.6
5. **Is risk control rejecting?** — `grep "REJECTED\|halted" logs/app.log`
   - Possible triggers: daily drawdown exceeded, position count limit, rate limit

---

## Appendix: Quick Reference Card

```
┌─────────────────────────────────────────────────────────┐
│              pulseTrader Operations Quick Reference      │
├─────────────────────────────────────────────────────────┤
│  Start:   ./run.sh trade (auto-loads trading.toml)      │
│  Monitor: ./run.sh cli (REPL) or embedded REPL in trade │
│  MCP:     ./run.sh mcp — LLM clients via control socket │
│  Stop:    Ctrl+C (<1s graceful shutdown)                │
│  Test:    ./run.sh test (583 unit tests)                │
│  Logs:    tail -f logs/*.log                            │
├─────────────────────────────────────────────────────────┤
│  .env:         PULSE_NETWORK / API Key / Proxy          │
│  trading.toml: Strategy params / Risk / AI / testnet    │
│  Sub-accounts: Isolate risk, no withdrawal permission   │
│  Circuit breakers: Daily loss 2% halt / Total DD 5% stop│
├─────────────────────────────────────────────────────────┤
│  ✅ Testnet:  PULSE_NETWORK=testnet virtual funds test  │
│  ⚠️  Mainnet: PULSE_NETWORK=mainnet real money          │
│  ⚠️  Round-trip fees: 0.4%                              │
│  ⚠️  Latency ~250-500ms, don't compete on speed        │
│  ⚠️  Testnet first, then small capital live trading     │
└─────────────────────────────────────────────────────────┘
```

---

## 10. CFD (TradFi) — 黄金 CFD 实盘

> 自 2026-08-14 起，实盘方向从 BTC_USDT 永续合约转向 Gate.io 传统金融 CFD（黄金 `XAUUSD`）。
> API 调研与账户验证已完成（见 [CFD_TRADFI.md](CFD_TRADFI.md)），引擎扩展已实施（M15）：`MarketType::Cfd`、REST 轮询行情、CFD 风控、方向切换（`switch_direction` / REPL `switch`）。**默认不实盘**——`trading.toml` 的 `active_market = "futures"` 下 CFD 策略保持暂停，`switch cfd` 后才激活。

### 10.1 当前账户状态（主账户，2026-08-14 实测）

| 项目 | 值 |
|---|---|
| CFD 账户 | 已注册，`mt5_uid = 2017864` |
| 余额 / 净值 | **81.86 USD**（无持仓） |
| XAUUSD | 现货报价 ~4348 USD/oz，1 手 = 100 oz，最小 0.01 手 |
| 杠杆 | 可选 20 / 50 / 100 / 200 / 500 |
| API key 权限 | **CFD** 已勾选（futures 权限与 CFD 无关——两者是独立产品） |

### 10.2 操作要点

1. **资金划转**：USDT → CFD 账户走 `POST /api/v4/tradfi/transactions`
   （`asset=USDT`、`type=deposit`）；CFD 账户以 **USD** 结算
2. **行情**：CFD 无 WebSocket，靠 REST 轮询 ticker（~1s）+ 1m klines（~60s，首次立即回填 500 根）；`get_market XAUUSD` 可查
3. **策略兼容**：momentum / mean_reversion / supertrend（K 线驱动）可用；
   orderbook_scalper 无盘口数据，不可用
4. **下单语义**：volume 按「手」（0.01 步进），side `1=卖 / 2=买`，
   市价 `price_type=market`，限价 `price_type=trigger`
5. **风控**：名义价值 = volume × 100 × 价格；保证金 = 名义价值 / 杠杆；
   0.01 手 ≈ 4348 USD 名义 ≈ 8.7 USD 保证金（500× 下）
6. **未实施前手动验证**：可用任意 REST 客户端（签名方式同 `gate_auth.hpp`）
   调 `/tradfi/orders` 小额测试；实施后走 CLI/MCP `open` 命令
