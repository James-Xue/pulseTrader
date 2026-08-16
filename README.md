# pulseTrader

![Build](https://img.shields.io/badge/build-passing-brightgreen)
![Tests](https://img.shields.io/badge/tests-643%20passing-brightgreen)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-orange)

**AI-driven scalping framework with adaptive strategy tuning via real-time social signals.**

---

## Overview

pulseTrader is a C++20 quantitative trading framework purpose-built for high-frequency scalping on Gate.io. It combines a low-latency market data pipeline with a periodic AI analysis cycle that ingests live social and news signals, calls a Large Language Model, and automatically nudges strategy parameters in response to changing market conditions — all without interrupting the hot WebSocket path.

The framework ships four production-ready scalping strategies out of the box and provides a clean abstract base class for adding custom strategies. Risk management, position tracking, stop-loss / take-profit logic, and SQLite trade recording are first-class components, not afterthoughts. The design philosophy is depth over breadth: one exchange, done properly.

**Milestones M1–M15 achieved** — all 9 layers operational with full spot + futures dual-market support plus switchable TradFi/CFD gold (XAUUSD), TOML configuration, SQLite trade recording, EndpointRouter for market-type-aware routing, leverage-aware risk management, Gate.io testnet support (mainnet WS for market data + testnet REST for virtual fund trading), graceful shutdown (Ctrl+C exits in <1s via io_context stop + curl abort callback + ProxyTunnel poll-based cleanup), and a complete trading engine wiring all layers into a single runnable process. The WebUI was removed on the `headless` branch and replaced by a **control plane**: an embedded CLI REPL, a remote-attach `cli` REPL, an MCP (Model Context Protocol) server for LLM clients, and a JSON-RPC control socket. Single-instance enforcement prevents two engines from trading at once, and a systemd user service provides auto-start + crash restart.

---

## Architecture Overview

pulseTrader is organised into nine vertical layers, each with a single well-defined responsibility:

| Layer | Name | Depends On | Summary |
|---|---|---|---|
| 1 | Exchange | — | Gate.io REST (HMAC-SHA512) + WebSocket client with auto-reconnect and proxy tunnel |
| 2 | Logging & Monitoring | — | Per-module async logging, trade recorder, metrics (cross-cutting) |
| 3 | Market Data | L1 | Order book reconstruction, K-line ring buffer, lock-free ticker cache |
| 4 | AI Analysis | L1 | Twitter/news ingestion, prompt assembly, LLM call, param deltas |
| 5 | Heartbeat Scheduler | L4 | 5-minute AI analysis clock, async task queue |
| 6 | Strategy Engine | L3, L5 | Multi-strategy manager, abstract base, signal aggregator |
| 7 | Risk Management | L6 | Order gate, position limits, stops, drawdown circuit breaker |
| 8 | Order Execution | L7, L1 | Order submission, WS + REST order tracking, execution reports |
| 9 | Control Plane | All | CLI REPL (embedded + remote `cli`), stdio MCP server, JSON-RPC control socket (TCP 127.0.0.1:8081) |

For the full architecture document including module responsibilities, key files, threading model, and design rationale, see [docs/architecture.md](docs/architecture.md).

---

## Key Features

- **5-minute AI heartbeat** — A dedicated background scheduler fires every 5 minutes, collects social and news context, calls an LLM (OpenAI GPT-4o or Anthropic Claude), and applies the resulting parameter deltas to live strategies without locking the market data thread.
- **Real-time social signal ingestion** — Streams tweets via X API v2 filtered stream and polls NewsAPI / CryptoPanic for crypto headlines, both fed directly into each AI prompt.
- **Four built-in scalping strategies** — `MomentumScalper` (EMA crossover), `OrderBookScalper` (bid/ask imbalance), `MeanReversionScalper` (Bollinger Band reversion), and `SuperTrendScalper` (ATR trend reversal), each running on its own `std::jthread`.
- **Weighted signal aggregation** — When multiple strategies are active, a `SignalAggregator` combines their signals using per-strategy confidence weights updated after each AI cycle.
- **Gate.io spot + futures integration** — Native REST (HMAC-SHA512 signed) and WebSocket channels for both spot and USDT perpetual futures, with EndpointRouter for market-type-aware routing, incremental order book updates, proxy tunnel support, and dual-market infrastructure (per-market REST/WS/Feed/Executor/Tracker).
- **Testnet support** — `PULSE_NETWORK=testnet` env switch routes REST API to Gate.io testnet (`api-testnet.gateapi.io`) for virtual fund trading while using mainnet WebSocket for identical real-time market data. TOML `testnet = true` in `[exchange]` section for file-driven config. Validator rejects spot strategies in testnet mode (futures-only).
- **Lock-free parameter hot-reload** — Strategy tunable values are stored as `std::atomic<double>`; `ParamAdvisor` updates them from the AI thread with zero locking overhead on the strategy side.
- **Layered risk management** — Fixed, trailing, and time-based stops; partial take-profit ladders; cross-strategy position limits; daily drawdown circuit breaker; token-bucket order rate limiter; futures-specific leverage limit and margin sufficiency checks with liquidation price estimation.
- **Fixed JSON schema for AI output** — The system prompt enforces a strict JSON schema for LLM responses, eliminating free-form parsing failures and making AI-driven parameter updates deterministic.
- **TOML configuration** — File-driven configuration via `trading.toml` with `from_env:` syntax for sensitive values, semantic validation, and sensible defaults for all fields.
- **SQLite trade recording** — 17-column `trades` table with WAL mode, 4 query APIs (by symbol/time/strategy, daily PnL), strategy tracking via `client_order_id`.
- **Control plane (Layer 9)** — Single `pulsetrader` binary with subcommands: `trade` (default; trading engine + JSON-RPC control socket + embedded REPL when stdin is a TTY), `cli` (remote-attach REPL over the control socket), and `mcp` (stdio MCP server bridging to the control socket for LLM clients like Claude Desktop / Claude Code). 17 control-plane methods (method names = MCP tool names): `get_status`, `get_account`, `get_positions`, `get_orders`, `list_strategies`, `get_strategy_params`, `set_strategy_param`, `open_order`, `close_position`, `cancel_order`, `halt_trading`, `resume_trading`, `get_risk`, `get_market`, `pause_strategy`, `resume_strategy`, `switch_direction`.
- **Runtime control** — Per-strategy pause/resume (`pause_strategy` / `resume_strategy`, atomic `setPaused`), manual trading halt (`halt_trading` / `resume_trading`), and live atomic strategy param get/set (`get_strategy_params` / `set_strategy_param`). Manual orders share the same `OrderFlowExecutor` as the signal aggregator.
- **Trading engine** — Single `./run.sh trade` command wires all 9 layers into a runnable process with graceful shutdown (<1s: SIGINT → curl abort callback cancels in-flight REST → reverse-order stop → io_context::stop → ProxyTunnel poll+relay cleanup → SQLite close → Logger flush).
- **Single-instance enforcement** — The engine takes an exclusive `flock` on `data/engine.lock` at startup; any second engine process (manual launch, another Claude session, a stale nohup) is refused immediately and exits. This prevents double trading — two engines placing orders independently caused the exchange position to drift from the engine view on 2026-08-16. The lock is kernel-managed, so crashes/SIGKILL leave no stale lock. Bypass with `PULSE_ALLOW_MULTI_INSTANCES=1` (not recommended).
- **Startup position reconciliation** — On startup the engine fetches real open positions from the exchange (`GET /futures/usdt/positions`, synced into the risk engine with entry/mark/liquidation prices, leverage and contract multiplier). Positions opened by a previous engine run or manually (e.g. SKHY shorts) are visible from the first second — displays, PnL and risk limits reflect true exposure, and a restart no longer forgets open positions. Synced ids use the `<symbol>_<Buy|Sell>_sync` form so they never collide with engine-opened ids. Note: because synced exposure counts toward risk limits, a large manual position can block new orders — raise `[risk]` limits if needed.
- **Configurable display timezone** — `[control] display_timezone` (`local` / `utc` / fixed offset like `"-04:00"` or `"+08:00"`) formats all human-readable timestamps in control-plane output (`*_str` fields in JSON-RPC responses, REPL tables, and every log line carries its UTC offset). Align the display with a phone app showing another timezone (e.g. US time vs Beijing time) so positions/orders can be cross-checked at a glance. Raw epoch fields stay untouched for machine consumption.

---

## Tech Stack

| Component | Library | Version |
|---|---|---|
| HTTP client | libcurl | ≥ 7.88 |
| TLS / signing | OpenSSL | ≥ 3.0 |
| JSON | nlohmann/json | ≥ 3.11 |
| Logging | spdlog | ≥ 1.12 |
| String formatting | fmt | ≥ 10.0 |
| Async I/O / timers | asio (standalone) | ≥ 1.28 |
| WebSocket | websocketpp | ≥ 0.8.2 |
| Config | toml11 | ≥ 4.0 |
| SQLite | SQLiteCpp | ≥ 3.3 |
| Testing | GTest | ≥ 1.14 |
| Dependency manager | vcpkg | latest |
| Build system | CMake | ≥ 3.20 |

---

## Project Structure

```
pulseTrader/
├── apps/
│   └── pulsetrader/        # Single binary: trade (default) / cli / mcp subcommands
├── cmake/                  # CMake helper modules
├── docs/                   # Architecture, operational guide, API documentation
├── src/
│   ├── ai/                 # Layer 4 — AI analysis pipeline
│   ├── app/                # Application-level helpers
│   ├── control/            # Layer 9 — Control plane (JSON-RPC server, REPL, MCP, OrderFlowExecutor)
│   ├── core/               # Config, types, errors, result
│   ├── exchange/           # Layer 1 — Gate.io REST + WebSocket
│   ├── execution/          # Layer 8 — Order executor + tracker
│   ├── heartbeat/          # Layer 5 — AI scheduler + task queue
│   ├── logging/            # Layer 2 — spdlog async logging
│   ├── market/             # Layer 3 — Market data pipeline
│   ├── risk/               # Layer 7 — Risk management (6 modules)
│   ├── strategy/           # Layer 6 — Strategy engine (4 strategies)
│   └── trade_recorder/     # SQLite trade recording
├── tests/
│   ├── unit/               # Unit tests (GTest)
│   └── integration/        # Integration tests
├── third_party/
│   └── websocketpp/        # websocketpp (git submodule)
├── tools/                  # Standalone test programs (smoke tests)
├── CMakeLists.txt
├── run.sh                  # Convenience runner script
├── trading.toml.example    # Example TOML configuration
├── vcpkg.json
└── LICENSE
```

---

## Getting Started

### Prerequisites

- **CMake** ≥ 3.20
- **vcpkg** (with `VCPKG_ROOT` environment variable set), **or** on Linux: apt-installed `libasio-dev`, `libwebsocketpp-dev`, `libsqlitecpp-dev` + vendored `third_party/websocketpp`
- A **C++20-capable compiler** (GCC ≥ 12, Clang ≥ 15, or MSVC ≥ 19.34)
- A **Gate.io API key and secret** with spot trading permissions
- An **OpenAI API key** (GPT-4o) or **Anthropic API key** (Claude) for the AI analysis layer *(optional)*
- An **X (Twitter) API v2 bearer token** for social signal ingestion *(optional)*

### Build

```bash
# 1. Clone the repository
git clone https://github.com/James-Xue/pulseTrader.git
cd pulseTrader

# 2. Configure (vcpkg will download and build all dependencies automatically)
cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# 3. Build
cmake --build build --config Release -j$(nproc)

# 4. Run tests (643 tests)
ctest --test-dir build --output-on-failure
```

<details>
<summary><strong>Linux build without vcpkg</strong> (apt + vendored websocketpp)</summary>

```bash
# Install system dependencies
sudo apt install libasio-dev libwebsocketpp-dev libsqlitecpp-dev \
                 libcurl4-openssl-dev libssl-dev nlohmann-json3-dev \
                 libspdlog-dev libfmt-dev libgtest-dev libtoml11-dev

# Configure (no vcpkg toolchain file needed)
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DPULSE_ENABLE_SQLITE=ON

# Build
cmake --build build -j$(nproc)
```
</details>

Optional CMake flags:

| Flag | Default | Description |
|---|---|---|
| `-DPULSE_ENABLE_SQLITE=ON` | OFF | Build SQLite trade recorder |

### Configuration

Create a `.env` file in the project root with your credentials:

```bash
# Network mode: "mainnet" (real funds) or "testnet" (virtual funds)
PULSE_NETWORK=testnet

# Mainnet API Key (https://www.gate.io/myaccount/api_key_manage)
GATE_MAINNET_API_KEY=your_mainnet_key
GATE_MAINNET_API_SECRET=your_mainnet_secret

# Testnet API Key (https://fx-testnet.gateio.ws)
GATE_TESTNET_API_KEY=your_testnet_key
GATE_TESTNET_API_SECRET=your_testnet_secret

# Proxy (optional)
HTTPS_PROXY=http://127.0.0.1:7897
HTTP_PROXY=http://127.0.0.1:7897

# Control plane (optional; default 8081)
PULSE_CONTROL_PORT=8081
```

For strategy parameters, risk limits, and AI settings, copy and edit the example TOML config:

```bash
cp trading.toml.example trading.toml
# Edit trading.toml to configure symbols, strategies, risk limits, etc.
```

Control-plane timestamps are shown in the machine's local timezone by default. To match a phone app showing another timezone (e.g. US time vs Beijing time), set `[control] display_timezone` in `trading.toml`:

```toml
[control]
display_timezone = "local"   # machine local time (default)
# display_timezone = "utc"              # UTC
# display_timezone = "-04:00"           # fixed offset, e.g. US Eastern summer time
# display_timezone = "+08:00"           # Beijing time, explicit
```

Applies to the `*_str` human-readable timestamp fields in JSON-RPC responses and the REPL `positions`/`orders` tables; raw epoch fields remain unchanged.

### Run

**Preferred: systemd user service** (auto-start at boot, crash restart, journald logs):

```bash
systemctl --user start pulsetrader         # start the engine
systemctl --user status pulsetrader        # status (active/running)
systemctl --user restart pulsetrader       # restart (e.g. after a rebuild)
journalctl --user -u pulsetrader -f        # live logs
```

The service sources `.env` and execs the same binary (`build_headless/.../pulsetrader --config trading.toml`). Boot-time startup is enabled via `loginctl enable-linger`. Because the engine refuses to start when `data/engine.lock` is held, at most one engine can ever run — the service and any manual launch cannot double-trade.

**Manual launch** (foreground, same binary):

```bash
# Start the trading engine (all 9 layers, auto-loads trading.toml if present).
# When stdin is a TTY an embedded REPL is enabled — type 'help' for commands.
./run.sh trade

# Start with explicit config
./run.sh trade --config trading.toml

# Attach a remote REPL to a running engine over the control socket
./run.sh cli

# Run the stdio MCP server (bridges to the running engine's control socket)
./run.sh mcp

# Smoke test tools
./run.sh rest        # Test REST connection
./run.sh ws          # Test WebSocket real-time data
./run.sh market      # Test L3 market data pipeline
./run.sh strategy    # Test strategy engine with mock data
./run.sh ai --mock   # Test AI pipeline (no real LLM call)
./run.sh test        # Run all 643 unit tests
```

### MCP Client Configuration

The `mcp` subcommand serves the control plane's 17 methods as MCP tools over stdio — usable from LLM clients such as Claude Desktop or Claude Code. Use absolute paths:

```bash
# Claude Code
claude mcp add pulsetrader -- /abs/path/build/apps/pulsetrader/pulsetrader mcp --config /abs/path/trading.toml
```

```json
// claude_desktop_config.json
{
  "mcpServers": {
    "pulsetrader": {
      "command": "/abs/path/build/apps/pulsetrader/pulsetrader",
      "args": ["mcp", "--config", "/abs/path/trading.toml"]
    }
  }
}
```

---

## How It Works

The core innovation in pulseTrader is the separation of the latency-critical market data path from the AI analysis cycle, connected by a lock-free parameter update mechanism.

```
                     ┌─────────────────────────────────────┐
Every 5 minutes      │   HeartbeatScheduler                │
                     │   (asio::steady_timer)               │
                     └──────────────┬──────────────────────┘
                                    │ enqueue OnBeat
                                    ▼
                     ┌─────────────────────────────────────┐
Background thread    │   TaskQueue  (priority queue)        │
                     └──────────────┬──────────────────────┘
                                    │
                                    ▼
                     ┌─────────────────────────────────────┐
AI I/O (1–5 sec)     │   AIAnalyzer                        │
                     │   TwitterFeed + NewsFeed             │
                     │   PromptBuilder + AIClient           │
                     │   → AnalysisResult (JSON)            │
                     └──────────────┬──────────────────────┘
                                    │
                                    ▼
                     ┌─────────────────────────────────────┐
                     │   ParamAdvisor                      │
                     │   validates deltas, writes atomics  │
                     └──────────────┬──────────────────────┘
                                    │ std::atomic<double> store
                                    ▼
                     ┌─────────────────────────────────────┐
Hot path             │   StrategyManager                   │
(no locking)         │   MomentumScalper                   │
                     │   OrderBookScalper                  │
                     │   MeanReversionScalper              │
                     └──────────────┬──────────────────────┘
                                    │ signals
                                    ▼
                     ┌─────────────────────────────────────┐
                     │   RiskManager → OrderExecutor       │
                     │   TradeRecorder + MetricsCollector  │
                     └─────────────────────────────────────┘
```

The WebSocket thread and strategy threads never wait on AI I/O. The AI cycle completes asynchronously on the background worker thread and atomically updates strategy parameters. Strategies pick up the new values on their next tick with a single atomic load.

---

## Milestones

| # | Milestone | Status |
|---|---|---|
| M1 | End-to-end Exchange → Market Data → Execution pipeline | ✅ |
| M2 | Automatic trading: Market Data → Strategy → Risk → Execution | ✅ |
| M3 | AI adaptive — strategy parameters auto-tune every 5 min | ✅ |
| M4 | Complete product — all 9 layers operational, control plane (REPL + MCP + JSON-RPC socket) | ✅ |
| M5 | Trading engine — 9-layer wiring into runnable process | ✅ |
| M6 | TOML config — file-driven configuration, validation, 46 tests | ✅ |
| M7 | SQLite trade recorder — 17-column schema, 4 queries, 27 tests | ✅ |
| M8 | Futures config foundation — enums, fields, error codes, 18 tests | ✅ |
| M9 | EndpointRouter + WS ping/pong fix | ✅ |
| M10 | Futures market data (mark_price, funding_rate, dual MarketFeed) | ✅ |
| M11 | Futures risk & PnL (leverage, margin, liquidation) | ✅ |
| M12 | Futures execution + dual-market wiring | ✅ |
| M13 | Testnet support — `PULSE_NETWORK` env switch, testnet URL override, validator guard | ✅ |
| M14 | Risk-gate hardening — single-evaluation order flow (kills the 3002 reject loop + reservation leak), futures contract-multiplier notional (quanto), symmetric long/short fill tracking | ✅ |
| M15 | Dual-direction trading (CFD gold + crypto futures, runtime-switchable) — `MarketType::Cfd`, TradFi REST paths, MT5 order schema verified live | 🚧 in progress |

---

## Recent Changes (2026-08-15)

- **Dual-direction trading (M15)** — the engine now runs two runtime-switchable trading directions: TradFi CFD gold (`XAUUSD`) and crypto futures (`BTC_USDT`), with a **single active direction** at any time. Switching (`switch_direction` method / REPL `switch <futures|cfd>` / MCP tool) pauses the other direction's strategies ("策略停跑"), cancels its open orders ("挂单全撤"), and resumes the new direction's; open positions stay until manually closed. The switch is ephemeral — restart returns to `active_market` in trading.toml (**default `"futures"`: CFD never trades until explicitly switched**).
  - `MarketType::Cfd` third market type (parsers, config plumbing, `active_market`, `risk.max_leverage` 125 → 500, testnet + orderbook_scalper rejected on CFD), error codes `71xx` + `InactiveMarket 3008`
  - L1+L8: `EndpointRouter` `/api/v4/tradfi/*` paths; `GateRestClient` 11 TradFi methods (symbols/tickers/klines/detail/assets/positions/orders/close/transfer); `OrderExecutor::buildOrderBody` static + MT5-style CFD body (`side` 2=buy/1=sell, `volume` lots, `price_type` market/trigger); `OrderTracker` REST-poll-only mode (CFD has no private WS channel)
  - L3: `MarketFeed` REST-poll mode for CFD (ticker ~1s, 1m klines ~60s with 500-candle backfill, dedupe by open_time, `std::jthread`); `SymbolRegistry` CFD branch (`/tradfi/symbols/detail` → `contract_volume` as the quanto slot, min/step/max volume, price precision) + `mergeFrom` combining futures + CFD specs into one lookup
  - L7: `RiskManager::evaluateCfdOrder` — margin = volume × contract_volume × price / leverage (7101/7102); CFD positions close via the dedicated `/tradfi/positions/{id}/close` endpoint (not an order)
  - L9: `switch_direction` (17th method/tool), `get_market` market_type-aware feed selection, `open_order` defaults to the active direction, `status()` reports `active_market`, heartbeat shows CFD feed + USD balance
  - Live-API probe (`tools/test_gate_rest --tradfi`, 2026-08-15): **MT5-style order schema confirmed** (spot-style variant rejected `INVALID_ARGUMENT`); responses wrap under `data`/`data.list`; order fields `order_id`/`state`/`finished`/`time_setup`; contract spec verified (100 oz/lot, 0.01 min/step, 15 max, leverage 20–500); CFD balance withdrawn to 0.00 (two leftover 08-14 probe orders await cancellation after the market reopens — `NOT_IN_TRADE` during close)
- **Verification** — 632 tests green (595 + 37 new M15 tests).

## Recent Changes (2026-08-14)

- **Risk-gate single evaluation** — `OrderFlowExecutor` used to re-evaluate an order inside placement; a `Modified` (quantity-capped) order was then rejected against its own notional reservation, permanently exhausting the symbol budget (3002 reject loop). Evaluation now happens exactly once and the reservation is passed into placement — capped orders place correctly and failed placements release the reservation.
- **Futures contract-multiplier notional** — risk evaluation treats a 1-contract futures order as 1 base-currency unit (1 BTC_USDT contract ≈ 6.29 USDT was sized as 1 BTC ≈ 62.9k USDT). `OrderRequest.quanto_multiplier` is now populated from the contract registry fetched at startup (`SymbolRegistry`, 919 contracts loaded), so notional = qty × price × quanto.
- **Symmetric fill tracking** — a SELL fill with no long to close now opens a tracked short position (previously shorts were never recorded, leaving the risk gate blind to real short exposure); fills close opposite-direction positions first, then open the remainder.
- **Verification** — 595 tests green (591 + 4 new regressions). Live mainnet verification: signals now pass the risk gate with correct 6.29 USDT notional at full 1-contract quantity; the remaining blocker was an API-key permission (`futures write`), resolved by the product pivot below.
- **Product pivot: CFD (TradFi)** — trading direction moved from BTC_USDT perpetual futures to Gate.io's traditional-finance CFD product (gold, `XAUUSD`). The TradFi API was researched end-to-end and verified live against the account (symbols, tickers, contract specs, CFD account `mt5_uid 2017864`, 81.86 USD balance). See [docs/CFD_TRADFI.md](docs/CFD_TRADFI.md) for the full API survey and the implementation plan. The futures engine remains operational but is halted pending the CFD integration.

---

## Roadmap

- [x] **Trading engine** — All 9 layers wired into a single runnable process
- [x] **TOML configuration** — File-driven config with `from_env:` syntax and validation
- [x] **SQLite trade recorder** — Persistent trade history with strategy tracking
- [x] **Control plane** — CLI REPL (embedded + remote `cli`), stdio MCP server, and JSON-RPC control socket (replaces the WebUI on the `headless` branch)
- [x] **Futures support** — Gate.io USDT perpetual contracts (M10: market data, M11: risk/PnL, M12: execution + dual-market wiring)
- [x] **Testnet support** — `PULSE_NETWORK` env switch, testnet REST + mainnet WS, TOML `testnet = true`, validator guard (M13)
- [x] **Risk-gate hardening** — single-evaluation order flow, futures quanto notional, symmetric fill tracking (M14, 2026-08-14)
- [ ] **Dual-direction trading (M15)** — CFD (TradFi gold `XAUUSD`) + crypto futures as runtime-switchable directions. MarketType/config/exchange/execution foundation + live-API probe done (2026-08-15); L3 REST polling feed, L7 CFD risk, control-plane `switch_direction`, main.cpp wiring, tests pending. Phased plan in [docs/CFD_TRADFI.md](docs/CFD_TRADFI.md)
- [ ] **Backtesting engine** — Replay historical Gate.io tick data against any registered strategy with full order simulation
- [ ] **Paper trading mode** — Full dry-run simulation with live market data but no real order submission
- [ ] **P&L reporting** — Control-plane views with daily/weekly/monthly P&L, win rate, and profit factor
- [ ] **Additional exchange support** — Binance and OKX adapters behind the same Layer 1 interface
- [ ] **Reinforcement learning adapter** — Replace the LLM-based `ParamAdvisor` with an RL agent trained on historical fills
- [ ] **Portfolio-level optimisation** — Cross-symbol capital allocation using Kelly criterion and correlation-aware position sizing

---

## License

This project is licensed under the **GNU General Public License v3.0**. See the [LICENSE](LICENSE) file for the full terms.

---

## Disclaimer

pulseTrader is experimental software provided for educational and research purposes only. Algorithmic trading involves substantial financial risk. Past performance of any strategy does not guarantee future results. You may lose some or all of your capital. The authors and contributors accept no responsibility for financial losses incurred through the use of this software. Always test thoroughly in paper trading mode before deploying real capital. Never trade with money you cannot afford to lose.
