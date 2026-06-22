# pulseTrader Operational Guide: From Zero to Live Trading

> This document is intended for operators, explaining how to advance pulseTrader from its current state to a production-ready trading system.
>
> Last updated: 2026-06-20 (Ctrl+C graceful shutdown fix + WebUI token cache + auth-free mode)

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
| L6 | Strategy | EMA crossover / order book imbalance / Bollinger mean reversion | ✅ | 59 |
| L7 | Risk Management | Position management / drawdown protection / rate limiting / stop-loss/take-profit / futures leverage risk control | ✅ | 104 |
| L8 | Execution | Order lifecycle management (dual market) | ✅ | 26 |
| L9 | WebUI | uWebSockets dark-themed SPA monitoring dashboard | ✅ | 57 |

**503 tests all passing** | `main` branch only | Milestones M1–M13 all achieved

### Currently Available Commands

```bash
./run.sh trade     # Start trading main program (9 layers chained, auto-loads trading.toml)
./run.sh trade --config trading.toml  # Start with specified TOML config file
./run.sh rest      # Test Gate.io REST connection (public + private endpoints)
./run.sh ws        # Test WebSocket real-time market data + private channels
./run.sh market    # Test market data pipeline (WS → L3 components)
./run.sh strategy  # Test strategy engine (simulated market data driving 3 strategies)
./run.sh ai        # Test AI Pipeline (--mock mode, no real LLM calls)
./run.sh webui     # Start WebUI monitoring dashboard (browser http://localhost:8080)
./run.sh test      # Run all 503 unit tests
```

### Trading Main Program (Completed)

`apps/pulsetrader/main.cpp` (~630 lines) chains all 9 layers into a complete trading system:

- **Construction order**: L2 Logger → L1 Exchange → L3 Market → L7 Risk → L8 Execution → L6 Strategy → L4 AI → L5 Heartbeat → L9 WebUI
- **Signal flow**: StrategyManager → SignalAggregator → app callback (risk check → OrderExecutor → OrderTracker)
- **Order completion callback**: OrderTracker → PositionManager open/close + DrawdownGuard PnL update
- **Graceful shutdown**: SIGINT/SIGTERM → atomic stop flag → reverse-order shutdown (WebUI → Strategy → Market → WS io_context::stop → ProxyTunnel poll+relay join → TradeRecorder → Logger)
- **Strategy factory**: `create_strategy()` creates concrete strategy classes based on configured names (MomentumScalper / OrderBookScalper / MeanReversionScalper)
- **Default config**: 2 strategies running on BTC_USDT, AI disabled, WebUI listening on :8080, credentials read from `.env`

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
| **P&L Dashboard** | WebUI profit/loss statistics panel (daily/weekly/monthly P&L, win rate, profit/loss ratio) | 3–4h |

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
PULSE_WEBUI_TOKEN=your_webui_token
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
maxPositionNotional = 500            # Max position size 500 USDT
maxOpenPositions = 3                 # Max 3 simultaneous open positions
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

# --- WebUI Configuration ---
[webui]
enabled = true
bindAddress = "127.0.0.1"
port = 8080
authToken = ""                         # Empty string = no authentication (recommended for dev/testnet)
                                       # For production, set a token or read from .env:
                                       # authToken = "from_env:PULSE_WEBUI_TOKEN"
maxClients = 4
```

> **WebUI Authentication Notes**:
> - `authToken = ""` → no authentication, browser access works directly (suitable for testnet development)
> - `authToken = "xxx"` → first visit shows an input dialog; after entering, cached in localStorage, no re-entry on refresh
> - Can also bypass the dialog via URL parameter: `http://localhost:8080/?token=xxx`

### 4.3 Start Trading

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
# [INFO] WebUI: http://127.0.0.1:8080
# [INFO] Trading engine started. Press Ctrl+C to stop.
#
# Approximately 60 seconds after startup, the system begins printing a heartbeat log line every 60 seconds:
# [INFO] [heartbeat] uptime 1m00s | futures 100 tick/s  10 kline/s  80 ob/s | ws spot=n/a futures=connected | strategies 3/3 running | positions 0 (notional 0.00 USDT)
```

### 4.4 Monitor Operation

```bash
# Terminal 2: Open WebUI
# Browse to http://127.0.0.1:8080 (WebUI already configured in trading.toml)

# Or view logs directly
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
#   1. L9: Stop WebUI server
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

## 5. Key Parameter Tuning Guide

### 5.1 Strategy Parameters

| Parameter | Meaning | Tuning Direction |
|-----------|---------|------------------|
| `order_quantity` | Order size per trade | Start small (0.001 BTC), increase gradually after confirming profitability |
| `min_confidence` | Signal confidence threshold | Higher = more conservative (fewer trades but more precise); lower = more aggressive (more trades but more noise) |
| `poll_interval_ms` | Market data polling frequency | Lower = better latency but higher CPU usage. Recommended 100–500ms |
| `signal_aggregator_threshold` | Aggregated signal execution threshold | 0.6 = single strategy can trigger an order; with multi-strategy consensus, raise to 0.7+ |
| `signal_cooldown_sec` | Per-symbol signal cooldown | Prevents consecutive order placement. For scalping, recommended 15–60 seconds |

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

# 6. WebUI
# Open browser to http://127.0.0.1:8080 to view real-time status
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
│  Monitor: ./run.sh webui → http://localhost:8080        │
│  Stop:    Ctrl+C (~1s graceful shutdown)                │
│  Test:    ./run.sh test (537 unit tests)                │
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
