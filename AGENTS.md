# AGENTS.md — AI Coding Assistant Guidelines

> This file tells AI coding assistants (Claude Code, GitHub Copilot, Cursor, etc.)\
> how to work effectively within the pulseTrader codebase.

---

## Project in 30 Seconds

**pulseTrader** is a C++20 high-frequency scalping framework targeting Gate.io exclusively. It integrates real-time market data with AI-driven sentiment analysis (LLM every 5 minutes) to adaptively tune strategy parameters. Architecture is 9 vertical layers — read `docs/architecture.md` before making any structural changes.

```
L1 Exchange → L3 Market Data → L6 Strategy → L7 Risk → L8 Execution
L5 Heartbeat → L4 AI → ParamAdvisor (atomic writes to L6)
L2 Logging (cross-cutting) · L9 WebUI (cross-cutting, read-only, optional)
```

**Key property**: the market data hot path (L1→L3→L6→L7→L8) must never block on AI inference, disk I/O, or external network calls. The AI pipeline runs on an isolated background thread (L5 heartbeat worker) and communicates with L6 via lock-free `std::atomic` parameter writes.

---

## Build & Run

```bash
# Configure (first time)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# Optional features
cmake -B build -S . -DPULSE_ENABLE_WEBUI=ON -DPULSE_ENABLE_SQLITE=ON -DPULSE_ENABLE_TOML=ON
```

Dependencies are managed by **vcpkg** — never add system packages; always add to `vcpkg.json`.

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
| **File naming** | `snake_case.hpp` / `snake_case.cpp`. |
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

1. **Don't add Boost.** The project uses standalone `asio`, not `boost::asio`. `uWebSockets` was chosen over `crow`/`boost::beast` specifically to avoid a Boost dependency.
2. **Don't add new exchange abstractions.** pulseTrader targets Gate.io only — depth of integration over breadth.
3. **Don't put blocking I/O on the market data thread.** AI calls, file writes, and REST requests go on dedicated background threads.
4. **Don't parse LLM responses as free text.** AI output must conform to the fixed JSON schema in `analysis_result.hpp`. Validation failure → discard, keep old params.
5. **Don't add dependencies to `vcpkg.json` without checking the optional-feature flag.** New HTTP libraries, JSON parsers, etc. should reuse existing deps.

---

## Documentation

- `docs/architecture.md` — full 9-layer architecture, module reference, data flow, threading model
- `docs/highLevelArchitecture.md` — condensed visual overview
- `docs/howItWorks.md` — narrative walkthrough
- `docs/implementation-roadmap.md` — phased build order (L2→L1→L3→L8→L7→L6→L5+L4→L9)

When making architectural changes, update `architecture.md` to keep it as the source of truth.

---

## Git

- Commit messages: conventional style (`feat:`, `fix:`, `refactor:`, `docs:`, `test:`, `chore:`).
- Keep commits atomic — one logical change per commit.
- Don't commit API keys, secrets, or `.env` files. The `.gitignore` already covers `.claude/` and build artifacts.

---

## When in Doubt

Ask before assuming. The architecture document is the source of truth; if it's silent on a topic, check `docs/howItWorks.md` and `docs/implementation-roadmap.md` before guessing.
