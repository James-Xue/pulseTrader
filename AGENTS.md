# AGENTS.md — AI Coding Assistant Guidelines

> This file tells AI coding assistants (Claude Code, GitHub Copilot, Cursor, etc.)\
> how to work effectively within the pulseTrader codebase.

---

## Project in 30 Seconds

**pulseTrader** is a C++20 high-frequency scalping framework targeting Gate.io exclusively. It integrates real-time market data with AI-driven sentiment analysis (LLM every 5 minutes) to adaptively tune strategy parameters. Architecture is 9 vertical layers — read `docs/architecture.md` before making any structural changes.

```
L1 Exchange → L3 Market Data → L6 Strategy → L7 Risk → L8 Execution
L5 Heartbeat → L4 AI → ParamAdvisor (atomic writes to L6)
L2 Logging (cross-cutting) · L9 Control Plane (JSON-RPC socket + CLI REPL + MCP, cross-cutting)
```

**Key property**: the market data hot path (L1→L3→L6→L7→L8) must never block on AI inference, disk I/O, or external network calls. The AI pipeline runs on an isolated background thread (L5 heartbeat worker) and communicates with L6 via lock-free `std::atomic` parameter writes.

---

## Build & Run

```bash
# Configure with vcpkg (first time)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# Configure without vcpkg (Linux: apt + vendored websocketpp)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
      -DPULSE_ENABLE_SQLITE=ON

# Build
cmake --build build -j$(nproc)

# Run tests (669 tests)
ctest --test-dir build --output-on-failure

# Optional features
cmake -B build -S . -DPULSE_ENABLE_SQLITE=ON
```

Dependencies are managed by **vcpkg** (preferred) or **apt + vendored `third_party/`** (Linux alternative). On Linux, websocketpp is vendored in `third_party/`; do NOT add it to `vcpkg.json` if building without vcpkg.

## Run Modes

`./run.sh` supports `{trade|cli|mcp|rest|ws|market|strategy|ai|test}` (the `webui` mode was removed with the WebUI):

- `./run.sh trade` — trading engine (default subcommand `trade`); embeds a REPL when stdin is a TTY, and opens the JSON-RPC control socket (TCP 127.0.0.1:8081, `[control]` TOML section, `PULSE_CONTROL_PORT` env override)
- `./run.sh cli` — remote-attach REPL over the control socket (auto-loads `trading.toml`; the engine must be running)
- `./run.sh mcp` — stdio MCP server bridging to the control socket (auto-loads `trading.toml`), for LLM clients like Claude Desktop / Claude Code

**Production deployment**: the engine runs as the systemd user service `pulsetrader.service` (`~/.config/systemd/user/`, `loginctl enable-linger`) — auto-start at boot, `Restart=on-failure`, journald logs (`journalctl --user -u pulsetrader -f`). Restart after rebuilding: `systemctl --user restart pulsetrader`.

**Single instance**: `runTrade()` takes an exclusive `flock` on `data/engine.lock` and exits if another engine holds it. NEVER start a second engine (manual `./run.sh trade`, another session, a nohup) while one is running — two engines trade independently and the engine view drifts from the exchange position (2026-08-16 incident). `PULSE_ALLOW_MULTI_INSTANCES=1` bypasses the lock for deliberate multi-instance setups only. `cli`/`mcp` subcommands must NOT take the lock.

## Control Plane Rule

The control socket speaks newline-delimited JSON-RPC 2.0 over TCP. **Method names on the control socket ARE the MCP tool names** — keep them identical when adding a method. The 18 methods/tools are: `get_status`, `get_account`, `get_positions`, `get_orders`, `list_strategies`, `get_strategy_params`, `set_strategy_param`, `open_order`, `close_position`, `cancel_order`, `halt_trading`, `resume_trading`, `get_risk`, `get_market`, `pause_strategy`, `resume_strategy`, `switch_direction`, `get_signals`. REPL commands in `CommandParser` map to these methods. Manual orders and the signal aggregator both flow through `OrderFlowExecutor` (Layer 8). `get_signals` reads the `SignalBoard` (strategy signals + indicator snapshots + aggregator consensus); with `[strategy] signal_only = true` the aggregator output never reaches the executor while manual orders remain live (the sub-agent's path).

---

## Rule #1 — English Only

**All documentation, code comments, log messages, commit messages, and variable names must be written in English.** This is an open-source project; the audience is global. No Chinese in source files, headers, docs, or git history.

---

## Coding Conventions

| Rule | Detail |
|------|--------|
| **Language** | C++20. Use `std::jthread`, `std::stop_token`, `std::ranges`, concepts, `std::atomic<double>`. |
| **Namespace** | All code in `pulse::` or sub-namespaces (`pulse::exchange`, `pulse::market`, etc.). |
| **Headers** | `#pragma once` (not `#ifndef` guards). |
| **File naming** | **Filename must match the primary class name**: `OrderExecutor.hpp` / `OrderExecutor.cpp` for `class OrderExecutor`. Multi-type modules (e.g., `config.hpp`, `types.hpp`, `risk_types.hpp`) keep descriptive `snake_case` names. |
| **Include style** | `"pulse/layer/module.hpp"` for project headers, `<nlohmann/json.hpp>` for third-party. |
| **Error handling** | Use `pulse::PulseError` exception hierarchy for fatal errors; return `std::expected<T, PulseError>` (C++23) or `std::optional` + log for non-fatal. Never swallow errors silently. |
| **Logging** | Use `PULSE_LOG_INFO/WARN/ERROR(module, fmt, ...)` macros — never `std::cout`. |
| **Thread safety** | Hot path data structures: prefer lock-free (atomics, seqlock). Only use `std::mutex` when lock-free is impractical. Document thread-safety in header comments. |
| **Naming** | Classes: `PascalCase` (no underscores). Functions/methods: `camelCase` (no underscores). Member variables: `m_camelCase` prefix. Constants: `kPascalCase`. Private members use `m_` prefix, NOT trailing underscore. Struct data fields (pure-data containers like `OrderRequest`, `Position`, config structs) keep `snake_case`. |
| **Braces** | **Always use braces** for `if`, `else`, `for`, `while`, `do-while` — even for single-line bodies. No `if (x) return;` on one line. |
| **Yoda conditions** | Put the constant/literal on the **left** side of comparisons: `if (0 == status)` not `if (status == 0)`. Prevents accidental `=` assignment. |
| **Comments** | **Required and detailed.** Use numbered lists for multi-step logic. See Comment Style below. |

### Comment Style

Every non-trivial function, class, and block must have detailed comments explaining intent, not just mechanics. For multi-step logic, use a **numbered list with line breaks**:

```cpp
// Process incoming tick and update internal state:
// 1. Validate timestamp is within acceptable skew window
// 2. Update best bid/ask in the lock-free order book snapshot
// 3. Recalculate mid-price and spread
// 4. If spread exceeds threshold, flag as illiquid and skip strategy evaluation
// 5. Publish updated snapshot to downstream consumers via atomic store
void MarketDataProcessor::on_tick(const TickEvent& tick) {
    // ...
}
```

**Where to comment**:
- Every public function/method: document purpose, parameters, return value, and thread-safety guarantees
- Every class: document ownership, lifetime, and which layer it belongs to
- Every `if`/`switch` with more than 2 branches: explain the decision tree
- Every lock-free or atomic operation: explain the memory ordering choice
- Every error path: explain what failed and why we recover (or don't)

**Never** write comments that just restate the code (`// increment counter`). Explain **why**, not **what**.

---

## Layer Boundaries

Each layer communicates only with adjacent layers through narrow typed interfaces. **Never** bypass this:

- ✅ Strategy (L6) reads from Market Data (L3) via `TickerCache::load()` / `OrderBookManager::snapshot()`
- ❌ Strategy (L6) must NOT call `GateWsClient` directly
- ✅ AI (L4) writes to Strategy params via `ParamAdvisor::apply(AnalysisResult)`
- ❌ AI (L4) must NOT mutate `StrategyParams` fields directly

When adding a new capability, ask: **which layer owns this responsibility, and which interface should it expose?**

---

## Testing

- **Unit tests**: `tests/unit/test_<module>.cpp` — use GTest, no network I/O, run in `ctest`.
- **Integration tests**: `tests/integration/test_<flow>.cpp` — may hit Gate.io testnet or a mock server.
- **Smoke tools**: `tools/test_<feature>.cpp` — manual verification scripts, not in CTest, print to stdout.

All new code needs unit tests. Integration tests are required for any cross-layer flow.

---

## Things to Avoid

1. **Don't add Boost.** The project uses standalone `asio`, not `boost::asio`. `websocketpp` was chosen over `crow`/`boost::beast` specifically to avoid a Boost dependency. (The WebUI's uWebSockets/uSockets were removed on the `headless` branch; only `third_party/websocketpp` remains vendored.)
2. **Don't add new exchange abstractions.** pulseTrader targets Gate.io only — depth of integration over breadth.
3. **Don't put blocking I/O on the market data thread.** AI calls, file writes, and REST requests go on dedicated background threads.
4. **Don't parse LLM responses as free text.** AI output must conform to the fixed JSON schema in `analysis_result.hpp`. Validation failure → discard, keep old params.
5. **Don't add dependencies to `vcpkg.json` without checking the optional-feature flag.** New HTTP libraries, JSON parsers, etc. should reuse existing deps. Also note: Linux builds use apt + vendored `third_party/` — CMakeLists.txt changes for vcpkg targets will break Linux builds.

---

## Documentation

- `docs/architecture.md` — full 9-layer architecture, module reference, data flow, threading model
- `docs/highLevelArchitecture.md` — condensed visual overview
- `docs/howItWorks.md` — narrative walkthrough
- `docs/implementation-roadmap.md` — phased build order (L2→L1→L3→L8→L7→L6→L5+L4→L9)
- `src/control/` — Layer 9 control plane: `JsonRpcServer` (TCP socket), `CommandParser` (REPL), `McpServer` (stdio MCP), `ControlClient`, `EngineServices`, `OrderFlowExecutor`

When making architectural changes, update `architecture.md` to keep it as the source of truth.

---

## Git

- Commit messages: conventional style (`feat:`, `fix:`, `refactor:`, `docs:`, `test:`, `chore:`).
- Keep commits atomic — one logical change per commit.
- Don't commit API keys, secrets, or `.env` files. The `.gitignore` already covers `.claude/` and build artifacts.

---

## When in Doubt

Ask before assuming. The architecture document is the source of truth; if it's silent on a topic, check `docs/howItWorks.md` and `docs/implementation-roadmap.md` before guessing.
