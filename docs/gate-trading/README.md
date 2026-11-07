# gate Trading Strategy Documentation Directory (commit_my_life mirror)

> This directory is a **structural mirror plus an authoritative-document snapshot** of `~/1_Code/commit_my_life/0_note/gate-trading/`.
> Organised by market × machine × category (taskbook / strategy / thinking board / state / review / statistics / tools).
>
> ⚠️ **Make changes at the source** — this directory is a snapshot; commit_my_life is the authoritative source for the strategy documents.
> For progress on engine-ising the strategies (strategy → C++ code), see [eth/eth-grid-cpp-migration.md](eth/eth-grid-cpp-migration.md) and `src/grid/`.

## Directory Structure

```
docs/gate-trading/
├── gate-futures-trigger-order-tp-sl-notes_2026-08-16.md   # Root-level shared memo (the three price_orders interfaces)
├── gold/      # XAUUSD CFD scalping + SignalBoard factor board
│   └── joey-Z170I-PRO-GAMING/{taskbook,strategy}         # Primary operating machine
├── sndk/      # SNDK_USDT perpetual short-chasing grid
│   ├── james-MECHREVO/taskbook                        # v2.5 takeover version
│   └── joey-Z170I-PRO-GAMING/{review,strategy}            # v2.1 baseline
└── eth/    # ETH_USDT perpetual short-chasing grid (v2 = the spec for the GridManager C++ port)
    ├── james-MECHREVO/{strategy,taskbook}                 # Home of the v2 canonical version
    └── joey-Z170I-PRO-GAMING/review                   # v1 full-cycle review (lessons → input for the C++ port)
```

Empty machine directories (ZhangdeMacBook-Pro and others) are held by `.gitkeep` placeholders so the structure stays consistent with the source.
Engine strategy instances and the `[grid]` section in `trading.toml` are a separate kind of configuration **inside this repository** and do not live in this directory.

## Authoritative Document Quick Reference

| Market | Document | Purpose | C++ port status |
|--------|----------|---------|-----------------|
| Ethereum | [strategy/eth-subagent-strategy.md](eth/james-MECHREVO/strategy/eth-subagent-strategy.md) | **ETH grid v2 specification** (trend gate / spike freeze / ATR adaptation / protective line A placed first) | ✅ Engine-ised → GridManager (`src/grid/`) |
| Ethereum | [taskbook/eth-agent-taskbook.md](eth/james-MECHREVO/taskbook/eth-agent-taskbook.md) | v1.1 takeover taskbook (parameter baseline) | ✅ Parameters merged into the `[grid]` TOML |
| Ethereum | [review/eth-review-20260821-grid-v1.md](eth/joey-Z170I-PRO-GAMING/review/eth-review-20260821-grid-v1.md) | v1 full-cycle review: lessons such as the missing trend gate, 9103 notional and ghost positions | ✅ All lessons folded into the v2 rules |
| SanDisk | [strategy/sndk-subagent-strategy.md](sndk/joey-Z170I-PRO-GAMING/strategy/sndk-subagent-strategy.md) | Short-chasing grid v2.1 baseline (the source ETH v1 was mirrored from) | ⏳ Not engine-ised (still run by a sub-agent) |
| SanDisk | [taskbook/sndk-agent-taskbook.md](sndk/james-MECHREVO/taskbook/sndk-agent-taskbook.md) | v2.5 takeover version for the home machine | ⏳ Same as above |
| SanDisk | [review/sndk-review-20260819-155300.md](sndk/joey-Z170I-PRO-GAMING/review/sndk-review-20260819-155300.md) | Review of the turning point from the v1 fixed range to v2 dynamic anchoring | ⏳ Lessons already captured in the SNDK v2 rules |
| Gold | [strategy/xauusd-signal-board-design.md](gold/joey-Z170I-PRO-GAMING/strategy/xauusd-signal-board-design.md) | SignalBoard factor-board design specification | ✅ Engine-ised (M20, `src/strategy/signal/`) |
| Gold | [strategy/xauusd-subagent-strategy.md](gold/joey-Z170I-PRO-GAMING/strategy/xauusd-subagent-strategy.md) | XAUUSD CFD scalping rules baseline | ⏳ Not engine-ised (run by a sub-agent) |
| Gold | [strategy/xauusd-strategy-changelog.md](gold/joey-Z170I-PRO-GAMING/strategy/xauusd-strategy-changelog.md) | Strategy version history | — |
| Gold | [taskbook/xauusd-agent-taskbook.md](gold/joey-Z170I-PRO-GAMING/taskbook/xauusd-agent-taskbook.md) | Gold sub-agent taskbook | ⏳ Not engine-ised |

## Content Not Copied (still in the commit_my_life source directory)

- **State JSON** (`*-agent-state.json`) — runtime artifacts, live machine-side state
- **Tool scripts** (`gate_ledger.py` / `sndk_grid.py` / `eth_watch.py` / `relay_*.sh` and so on) — they contain absolute machine paths and belong to the runtime environment; they will be retired gradually as the strategies are engine-ised
- **Thinking board** (`*-agent-thinking.md`) — round-by-round log, not authoritative rules
- **Review log** (20+ posts for Gold) — only the key turning-point reviews are mirrored
- **Statistics / session communication / locks and logs / __pycache__**

## Engine-isation Guide

- The documents in this directory are the "prose strategies"; `src/grid/` is the C++ implementation of ETH grid v2
- The rule-by-rule mapping is in [eth/eth-grid-cpp-migration.md](eth/eth-grid-cpp-migration.md)
- Grid control surface: `grid_start / grid_status / grid_pause / grid_resume / grid_stop` (available on all three channels: REPL / JSON-RPC / MCP)
