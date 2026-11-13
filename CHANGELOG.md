# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Qt client local account: the password derives an AES-256-GCM vault key
  (PBKDF2-HMAC-SHA256), and the unlocked profile screen selects the Gate.io
  network/market and stores the API credentials encrypted on disk.
- The Qt client now starts and owns the engine process, generating a
  per-run signal-only config in the user runtime directory.
- `get_account` reports every non-zero spot currency (`spot_balances`) with a
  best-effort USDT valuation from the ticker cache; the Qt dashboard lists
  those holdings with amount and value.
- `watch_symbols` streams a watchlist in the active market without creating
  strategy instances. The Qt profile screen edits it (persisted in the vault)
  and the Market page dropdown offers it automatically.
- The spot breakdown is opt-in (`include_spot_balances`, default false)
  because pricing unpriced coins costs REST calls on the shared mutex. The Qt
  client exposes it as a Settings switch, so users who only need the balance
  can turn the extra work off.
- The Qt dashboard separates spot, CFD and futures into tabs, the way the
  Gate.io app does; each tab shows that market's balances, open positions and
  open orders. The Positions and Orders pages gain the matching market filter.
- `get_orders` stamps `market_type` on every active order and recent fill, so
  the client can attribute an order to a market without guessing from the
  symbol.
- Positions and orders poll at 2 Hz (balances at 0.2 Hz, because
  `get_account` holds the shared REST mutex that order placement needs). The
  engine's tracker reconcile is single-flight, throttled to one round per 3 s
  measured from completion, and holds the shared REST mutex only around each
  individual status poll, so the higher UI cadence neither floods the exchange
  nor lengthens the order-placement critical section.
- **GoldTrader (L7)** — an exit-only USD dynamic stop supervisor for every
  open XAUUSD CFD position (including positions opened manually in the Gate
  app and imported by `sync_positions`). Rule ladder: -8 USD hard stop,
  break-even after +2 USD peak profit, a 1-minute SuperTrend candidate with a
  1 USD buffer, and a 5 USD peak give-back that closes at market; stops only
  tighten. Every tightened stop is mirrored to the exchange-native `price_sl`
  so protection survives a process crash (the exchange's current stop is
  reconciled every pass, so a removed or loosened stop is restored). While
  armed it is the sole engine-side exit authority for XAUUSD: the
  percentage-based `ProtectiveExitManager` skips the symbol, so its trailing
  stop and partial take-profit ladder cannot act on the same position.
  Loaded from the top-level `[gold_trader]` section (enabled in the live
  config); no strategy instance is required. See
  `docs/strategies/gold-trader.md`.
- `get_gold_trader` control-plane method / MCP tool (34 total) exposes the
  L7 supervisor's armed state, thresholds and tracked positions. The Qt Gold
  Traders page renders it as an "L7 dynamic stop" status card (Armed/Disabled,
  USD thresholds, and per-position mark / PnL / peak / effective stop /
  exchange stop).
- CFD 1-minute klines are polled on the wall-clock minute boundary and
  `KlineBuffer::updateOrPush` refreshes the newest bar in place, so each bar
  converges to its final OHLC instead of staying frozen at its first partial
  snapshot. `MarketFeed::lastKlineFetchMs` gives consumers a finality anchor;
  the gold trader's SuperTrend leg refuses bars that were never re-read after
  their close.

### Fixed

- Qt client retries the first control-socket connection when it is refused,
  which is the normal case while a newly spawned engine is binding its port.
- `modify_sl_tp` now addresses the exchange position id instead of the
  engine-local id (the PUT previously used an id Gate never issued). It also
  reads the live position before composing the request when the caller omits
  a field, so a stop tightening never reverts or clears a take-profit that was
  changed in the app since the last sync. Both `modify_sl_tp` and the CFD
  close path treat a 2xx TradFi business error (`{"label","message"}`) as a
  failure instead of recording success.

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

- 1061 automated tests green (`ctest --test-dir build`).

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
