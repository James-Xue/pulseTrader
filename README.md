# pulseTrader

![Build](https://img.shields.io/badge/build-passing-brightgreen)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)
![C++](https://img.shields.io/badge/C%2B%2B-20-orange)

**AI-driven scalping framework with adaptive strategy tuning via real-time social signals.**

---

## Overview

pulseTrader is a C++20 quantitative trading library purpose-built for high-frequency scalping on Gate.io. It combines a low-latency market data pipeline with a periodic AI analysis cycle that ingests live social and news signals, calls a Large Language Model, and automatically nudges strategy parameters in response to changing market conditions — all without interrupting the hot WebSocket path.

The framework ships three production-ready scalping strategies out of the box and provides a clean abstract base class for adding custom strategies. Risk management, position tracking, stop-loss / take-profit logic, and alerting are first-class components, not afterthoughts. The design philosophy is depth over breadth: one exchange, done properly.

---

## Architecture Overview

pulseTrader is organised into eight vertical layers, each with a single well-defined responsibility:

| Layer | Name | Depends On | Summary |
|---|---|---|---|
| 1 | Exchange | — | Gate.io REST (HMAC-SHA512) + WebSocket client with auto-reconnect |
| 2 | Logging & Monitoring | — | Per-module logging, trade recorder, metrics, alert dispatcher (cross-cutting) |
| 3 | Market Data | L1 | Order book reconstruction, K-line ring buffer, lock-free ticker cache |
| 4 | AI Analysis | L1 | Twitter/news ingestion, prompt assembly, LLM call, param deltas |
| 5 | Heartbeat Scheduler | L4 | 5-minute AI analysis clock, async task queue |
| 6 | Strategy Engine | L3, L5 | Multi-strategy manager, abstract base, signal aggregator |
| 7 | Risk Management | L6 | Order gate, position limits, stops, drawdown circuit breaker |
| 8 | Order Execution | L7, L1 | Order submission, WS + REST order tracking, execution reports |

For the full architecture document including module responsibilities, key files, threading model, and design rationale, see [docs/architecture.md](docs/architecture.md).

---

## Key Features

- **5-minute AI heartbeat** — A dedicated background scheduler fires every 5 minutes, collects social and news context, calls an LLM (OpenAI GPT-4o or Anthropic Claude), and applies the resulting parameter deltas to live strategies without locking the market data thread.
- **Real-time social signal ingestion** — Streams tweets via X API v2 filtered stream and polls NewsAPI / CryptoPanic for crypto headlines, both fed directly into each AI prompt.
- **Three built-in scalping strategies** — `MomentumScalper` (EMA crossover), `OrderBookScalper` (bid/ask imbalance), and `MeanReversionScalper` (Bollinger Band reversion), each running on its own `std::jthread`.
- **Weighted signal aggregation** — When multiple strategies are active, a `SignalAggregator` combines their signals using per-strategy confidence weights updated after each AI cycle.
- **Deep Gate.io integration** — Native support for Gate.io spot and futures: REST (HMAC-SHA512 signed), WebSocket channels (order book, trades, tickers, K-lines, private orders), incremental order book updates, and funding rate feeds.
- **Lock-free parameter hot-reload** — Strategy tunable values are stored as `std::atomic<double>`; `ParamAdvisor` updates them from the AI thread with zero locking overhead on the strategy side.
- **Layered risk management** — Fixed, trailing, and time-based stops; partial take-profit ladders; cross-strategy position limits; daily drawdown circuit breaker; token-bucket order rate limiter.
- **Fixed JSON schema for AI output** — The system prompt enforces a strict JSON schema for LLM responses, eliminating free-form parsing failures and making AI-driven parameter updates deterministic.
- **Structured observability** — Per-module `spdlog` async logging, CSV/SQLite trade recording, rolling PnL/Sharpe/drawdown metrics, and webhook/Telegram alerts.

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
| Testing | GTest | ≥ 1.14 |
| Config *(optional)* | toml11 | ≥ 3.8 |
| SQLite *(optional)* | SQLiteCpp | ≥ 3.3 |
| Dependency manager | vcpkg | latest |
| Build system | CMake | ≥ 3.20 |

---

## Project Structure

```
pulseTrader/
├── apps/               # Executable entry points (main programs)
├── cmake/              # CMake helper modules and toolchain files
├── demos/              # Standalone demo programs
├── docs/               # Architecture and API documentation
├── include/
│   └── pulse/
│       ├── exchange/   # Layer 1 headers
│       ├── logging/    # Layer 2 headers (cross-cutting)
│       ├── market/     # Layer 3 headers
│       ├── ai/         # Layer 4 headers
│       ├── heartbeat/  # Layer 5 headers
│       ├── strategy/   # Layer 6 headers
│       ├── risk/       # Layer 7 headers
│       └── execution/  # Layer 8 headers
├── src/                # Implementation files (mirrors include/pulse/)
├── tests/              # Unit and integration tests (GTest)
├── scripts/            # Build, deploy, and utility scripts
├── logs/               # Runtime log output (git-ignored)
├── CMakeLists.txt
├── vcpkg.json
└── LICENSE
```

---

## Getting Started

### Prerequisites

- **CMake** ≥ 3.20
- **vcpkg** (with `VCPKG_ROOT` environment variable set)
- A **C++20-capable compiler** (GCC ≥ 12, Clang ≥ 15, or MSVC ≥ 19.34)
- A **Gate.io API key and secret** with spot/futures trading permissions
- An **OpenAI API key** (GPT-4o) or **Anthropic API key** (Claude) for the AI analysis layer
- An **X (Twitter) API v2 bearer token** for social signal ingestion *(optional but recommended)*

### Build

```bash
# 1. Clone the repository
git clone https://github.com/your-username/pulseTrader.git
cd pulseTrader

# 2. Configure (vcpkg will download and build all dependencies automatically)
cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# 3. Build
cmake --build build --config Release -j$(nproc)

# 4. Run tests
ctest --test-dir build --output-on-failure
```

To enable optional features:

```bash
cmake -B build \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
      -DPULSE_ENABLE_TOML=ON \
      -DPULSE_ENABLE_SQLITE=ON
```

### Configuration

Copy the example environment file and fill in your credentials:

```bash
cp .env.example .env
```

| Variable | Description |
|---|---|
| `GATE_API_KEY` | Gate.io REST/WS API key |
| `GATE_API_SECRET` | Gate.io API secret (used for HMAC-SHA512 signing) |
| `OPENAI_API_KEY` | OpenAI API key (used by `AIClient` when backend is `openai`) |
| `ANTHROPIC_API_KEY` | Anthropic API key (used by `AIClient` when backend is `claude`) |
| `AI_BACKEND` | `openai` or `claude` |
| `AI_MODEL` | Model identifier, e.g. `gpt-4o` or `claude-opus-4-5` |
| `TWITTER_BEARER_TOKEN` | X API v2 bearer token for `TwitterFeed` |
| `TELEGRAM_BOT_TOKEN` | Telegram Bot API token for `AlertManager` *(optional)* |
| `TELEGRAM_CHAT_ID` | Telegram chat ID to send alerts to *(optional)* |
| `LOG_LEVEL` | Global log level: `trace`, `debug`, `info`, `warn`, `error` |

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

## Roadmap

- [ ] **Backtesting engine** — Replay historical Gate.io tick data against any registered strategy with full order simulation
- [ ] **Additional exchange support** — Binance and OKX adapters behind the same Layer 1 interface
- [ ] **Web monitoring dashboard** — Real-time metrics, open positions, and AI analysis history via a local HTTP server
- [ ] **Reinforcement learning adapter** — Replace the LLM-based `ParamAdvisor` with an RL agent trained on historical fills
- [ ] **Paper trading mode** — Full dry-run simulation with live market data but no real order submission
- [ ] **Portfolio-level optimisation** — Cross-symbol capital allocation using Kelly criterion and correlation-aware position sizing
- [ ] **Docker / systemd deployment** — Production-ready container image and service unit file

---

## License

This project is licensed under the **GNU General Public License v3.0**. See the [LICENSE](LICENSE) file for the full terms.

---

## Disclaimer

pulseTrader is experimental software provided for educational and research purposes only. Algorithmic trading involves substantial financial risk. Past performance of any strategy does not guarantee future results. You may lose some or all of your capital. The authors and contributors accept no responsibility for financial losses incurred through the use of this software. Always test thoroughly in paper trading mode before deploying real capital. Never trade with money you cannot afford to lose.
