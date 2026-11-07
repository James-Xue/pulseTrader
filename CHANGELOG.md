# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-11-07

First tagged release. The engine, the control plane and the desktop client are
usable; the AI tuning loop is present but **experimental and unverified**, and
no strategy has demonstrated a robust positive expectancy (see the Project
status table in the README).

### Added

- **Nine-layer C++20 engine** — exchange, logging, market data, AI analysis,
  heartbeat, strategy, risk, execution and control plane, with strict
  adjacent-layer interfaces.
- **Gate.io integration** — signed REST (HMAC-SHA512) and WebSocket channels
  for spot and USDT-margined futures, incremental order-book reconstruction,
  kline ring buffers, proxy tunnelling.
- **Six scalping strategies** — Momentum, OrderBook, MeanReversion, SuperTrend,
  EthScalper and EmaResonance, each with a written rule document under
  `docs/strategies/`.
- **Risk and execution** — fixed/trailing/time stops, take-profit ladders,
  per-market notional caps, drawdown guard, token-bucket rate limiting,
  maker-first order flow, startup and periodic position reconciliation.
- **Control plane (Layer 9)** — newline-delimited JSON-RPC 2.0 on a TCP socket,
  32 methods whose names double as MCP tool names, an embedded REPL, a
  remote-attach `cli`, and a stdio `mcp` bridge for LLM clients.
- **Optional control-plane TLS and token authentication** — off by default;
  SHA-256 digest comparison with `CRYPTO_memcmp`, 5 s non-blocking handshake
  budget, `9105`/`9106` error codes.
- **Qt 6.12 desktop client** (`client/qt`) — read-only console over the JSON-RPC
  socket, QML/Qt Quick on the Basic style with a custom macOS-like light/dark
  theme, three sockets (fast/rest/slow), keyed list models, and transport unit
  tests.
- **Backtesting and data tooling** — 1-minute kline replay for any USDT-M
  symbol, kline store, parameter sweeps, and Gate/Binance history fetchers.
- **Zero-key demo fixture** — `tools/demo_engine.py` and `docker/demo.Dockerfile`
  serve the real wire contract with sample payloads so the protocol can be
  exercised without credentials.

### Tested

- 1054 automated tests green (`ctest --test-dir build`).

### Known limitations

- **No proven edge.** A 480-combination parameter sweep on 1-minute BTC found no
  robust positive expectancy; the A/B weighting experiment was stopped before a
  verdict.
- **No paper-trading engine.** The demo fixture serves fixed sample data; the
  backtester replays stored klines and does not model funding, margin,
  liquidation or slippage.
- **Not safe to leave unattended.** Eight blockers are documented in
  `docs/btc-auto-trading-gap-analysis-2026-10-04.md`.
- **Single exchange** (Gate.io) by design.
- **Documentation is partly Chinese.** Several strategy and operator documents
  are being translated; see the project roadmap.

[Unreleased]: https://github.com/James-Xue/pulseTrader/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/James-Xue/pulseTrader/releases/tag/v0.1.0
