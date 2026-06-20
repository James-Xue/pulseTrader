# pulseTrader

![Build](https://img.shields.io/badge/build-passing-brightgreen)
![Tests](https://img.shields.io/badge/tests-497%20passing-brightgreen)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-orange)

**AI-driven scalping framework with adaptive strategy tuning via real-time social signals.**

---

## Overview

pulseTrader is a C++20 quantitative trading framework purpose-built for high-frequency scalping on Gate.io. It combines a low-latency market data pipeline with a periodic AI analysis cycle that ingests live social and news signals, calls a Large Language Model, and automatically nudges strategy parameters in response to changing market conditions — all without interrupting the hot WebSocket path.

The framework ships three production-ready scalping strategies out of the box and provides a clean abstract base class for adding custom strategies. Risk management, position tracking, stop-loss / take-profit logic, and SQLite trade recording are first-class components, not afterthoughts. The design philosophy is depth over breadth: one exchange, done properly.

**Milestones M1–M12 achieved** — all 9 layers operational with full spot + futures dual-market support, TOML configuration, SQLite trade recording, EndpointRouter for spot/futures routing, leverage-aware risk management, and a complete trading engine wiring all layers into a single runnable process.

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
| 9 | WebUI | All | uWebSockets dark-theme SPA dashboard, tiered polling (200ms–5min) |

For the full architecture document including module responsibilities, key files, threading model, and design rationale, see [docs/architecture.md](docs/architecture.md).

---

## Key Features

- **5-minute AI heartbeat** — A dedicated background scheduler fires every 5 minutes, collects social and news context, calls an LLM (OpenAI GPT-4o or Anthropic Claude), and applies the resulting parameter deltas to live strategies without locking the market data thread.
- **Real-time social signal ingestion** — Streams tweets via X API v2 filtered stream and polls NewsAPI / CryptoPanic for crypto headlines, both fed directly into each AI prompt.
- **Three built-in scalping strategies** — `MomentumScalper` (EMA crossover), `OrderBookScalper` (bid/ask imbalance), and `MeanReversionScalper` (Bollinger Band reversion), each running on its own `std::jthread`.
- **Weighted signal aggregation** — When multiple strategies are active, a `SignalAggregator` combines their signals using per-strategy confidence weights updated after each AI cycle.
- **Gate.io spot + futures integration** — Native REST (HMAC-SHA512 signed) and WebSocket channels for both spot and USDT perpetual futures, with EndpointRouter for market-type-aware routing, incremental order book updates, proxy tunnel support, and dual-market infrastructure (per-market REST/WS/Feed/Executor/Tracker).
- **Lock-free parameter hot-reload** — Strategy tunable values are stored as `std::atomic<double>`; `ParamAdvisor` updates them from the AI thread with zero locking overhead on the strategy side.
- **Layered risk management** — Fixed, trailing, and time-based stops; partial take-profit ladders; cross-strategy position limits; daily drawdown circuit breaker; token-bucket order rate limiter; futures-specific leverage limit and margin sufficiency checks with liquidation price estimation.
- **Fixed JSON schema for AI output** — The system prompt enforces a strict JSON schema for LLM responses, eliminating free-form parsing failures and making AI-driven parameter updates deterministic.
- **TOML configuration** — File-driven configuration via `trading.toml` with `from_env:` syntax for sensitive values, semantic validation, and sensible defaults for all fields.
- **SQLite trade recording** — 17-column `trades` table with WAL mode, 4 query APIs (by symbol/time/strategy, daily PnL), strategy tracking via `client_order_id`.
- **WebUI dashboard** — uWebSockets-powered dark-theme SPA with real-time monitoring, tiered polling (200ms–5min), bearer token auth, localhost-only binding.
- **Trading engine** — Single `./run.sh trade` command wires all 9 layers into a runnable process with graceful shutdown (SIGINT/SIGTERM → reverse-order stop → auto-close positions).

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
| WebUI server | uWebSockets | vendored |
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
│   └── pulsetrader/        # Trading engine entry point (main.cpp, 9-layer wiring)
├── cmake/                  # CMake helper modules
├── docs/                   # Architecture, operational guide, API documentation
├── frontend/               # WebUI SPA (index.html, style.css, app.js)
├── src/
│   ├── ai/                 # Layer 4 — AI analysis pipeline
│   ├── app/                # Application-level helpers
│   ├── core/               # Config, types, errors, result
│   ├── exchange/           # Layer 1 — Gate.io REST + WebSocket
│   ├── execution/          # Layer 8 — Order executor + tracker
│   ├── heartbeat/          # Layer 5 — AI scheduler + task queue
│   ├── logging/            # Layer 2 — spdlog async logging
│   ├── market/             # Layer 3 — Market data pipeline
│   ├── risk/               # Layer 7 — Risk management (6 modules)
│   ├── strategy/           # Layer 6 — Strategy engine (3 strategies)
│   ├── trade_recorder/     # SQLite trade recording
│   └── webui/              # Layer 9 — uWebSockets dashboard server
├── tests/
│   ├── unit/               # Unit tests (GTest)
│   └── integration/        # Integration tests
├── third_party/
│   ├── uWebSockets/        # Vendored uWebSockets source
│   └── uSockets/           # Vendored uSockets source
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
- **vcpkg** (with `VCPKG_ROOT` environment variable set)
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

# 4. Run tests (497 tests)
ctest --test-dir build --output-on-failure
```

Optional CMake flags:

| Flag | Default | Description |
|---|---|---|
| `-DPULSE_ENABLE_WEBUI=ON` | OFF | Build Layer 9 WebUI dashboard server |
| `-DPULSE_ENABLE_SQLITE=ON` | OFF | Build SQLite trade recorder |

### Configuration

Create a `.env` file in the project root with your credentials:

```bash
GATE_API_KEY=your_api_key
GATE_API_SECRET=your_api_secret
HTTPS_PROXY=http://127.0.0.1:7897    # optional, for proxy tunnel
HTTP_PROXY=http://127.0.0.1:7897     # optional
```

For strategy parameters, risk limits, and AI settings, copy and edit the example TOML config:

```bash
cp trading.toml.example trading.toml
# Edit trading.toml to configure symbols, strategies, risk limits, etc.
```

### Run

```bash
# Start the trading engine (all 9 layers)
./run.sh trade --config trading.toml

# Open WebUI dashboard (browser → http://localhost:8080)
./run.sh webui

# Smoke test tools
./run.sh rest        # Test REST connection
./run.sh ws          # Test WebSocket real-time data
./run.sh market      # Test L3 market data pipeline
./run.sh strategy    # Test strategy engine with mock data
./run.sh ai --mock   # Test AI pipeline (no real LLM call)
./run.sh test        # Run all 497 unit tests
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
| M4 | Complete product — all 9 layers operational, WebUI dashboard | ✅ |
| M5 | Trading engine — 9-layer wiring into runnable process | ✅ |
| M6 | TOML config — file-driven configuration, validation, 46 tests | ✅ |
| M7 | SQLite trade recorder — 17-column schema, 4 queries, 27 tests | ✅ |
| M8 | Futures config foundation — enums, fields, error codes, 18 tests | ✅ |
| M9 | EndpointRouter + WS ping/pong fix | ✅ |
| M10 | Futures market data (mark_price, funding_rate, dual MarketFeed) | ✅ |
| M11 | Futures risk & PnL (leverage, margin, liquidation) | ✅ |
| M12 | Futures execution + dual-market wiring | ✅ |

---

## Roadmap

- [x] **Trading engine** — All 9 layers wired into a single runnable process
- [x] **TOML configuration** — File-driven config with `from_env:` syntax and validation
- [x] **SQLite trade recorder** — Persistent trade history with strategy tracking
- [x] **WebUI dashboard** — Real-time uWebSockets monitoring SPA
- [x] **Futures support** — Gate.io USDT perpetual contracts (M10: market data, M11: risk/PnL, M12: execution + dual-market wiring)
- [ ] **Backtesting engine** — Replay historical Gate.io tick data against any registered strategy with full order simulation
- [ ] **Paper trading mode** — Full dry-run simulation with live market data but no real order submission
- [ ] **P&L dashboard** — WebUI panel with daily/weekly/monthly P&L, win rate, and profit factor
- [ ] **Additional exchange support** — Binance and OKX adapters behind the same Layer 1 interface
- [ ] **Reinforcement learning adapter** — Replace the LLM-based `ParamAdvisor` with an RL agent trained on historical fills
- [ ] **Portfolio-level optimisation** — Cross-symbol capital allocation using Kelly criterion and correlation-aware position sizing

---

## License

This project is licensed under the **GNU General Public License v3.0**. See the [LICENSE](LICENSE) file for the full terms.

---

## Disclaimer

pulseTrader is experimental software provided for educational and research purposes only. Algorithmic trading involves substantial financial risk. Past performance of any strategy does not guarantee future results. You may lose some or all of your capital. The authors and contributors accept no responsibility for financial losses incurred through the use of this software. Always test thoroughly in paper trading mode before deploying real capital. Never trade with money you cannot afford to lose.
