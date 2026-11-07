# MCP demo — drive the engine from an LLM client

The control plane exposes 32 methods, and **the method names are the MCP tool
names**. That means any MCP-capable client — Claude Code, Claude Desktop, or a
custom agent — can read the engine's state and (on a live engine) place manual
orders, without learning a second protocol.

This page is the demo script and the reproducible transcript.

## Run it

```bash
python3 tools/mcp_demo.py
```

The script starts the sample-data engine, starts `pulsetrader mcp` as a stdio
bridge, then speaks MCP to it: discover the protocol, list the tools, call a few.

No exchange keys, no funded account, no trading engine run. Only the
`pulsetrader` binary needs to be built:

```bash
cmake -B build -S . -DPULSE_ENABLE_SQLITE=ON && cmake --build build -j"$(nproc)"
```

## Transcript

```
$ server/discover
  supportedVersions: ['2026-07-28', '2025-11-25', '2025-06-18', ...]

$ tools/list
  32 tools: get_status, get_account, get_positions, get_orders, get_signals, grid_start …

$ tools/call get_status
  {"active_market": "futures", "network": "demo", "open_positions": 1,
   "strategies_running": 2, "strategies_total": 3, ...}

$ tools/call get_positions
  {"portfolio": {"net_exposure": 100000.0, "openPositionCount": 1, ...},
   "positions": [{"entry_price": 50000.0, "symbol": "BTC_USDT", ...}]}

$ tools/call get_orders
  {"activeOrders": [{"order_id": "demo-order-1", "order_type": "limit",
                     "price": 49000.0, "side": "Buy", "status": "open", ...}], ...}

$ tools/call get_signals
  {"aggregate": null, "signals": [{"confidence": 0.72, "type": "Buy", ...}]}
```

## Why it matters

- **One protocol, two surfaces.** The same 32 names drive the REPL, the JSON-RPC
  socket and MCP. There is no adapter layer to drift.
- **Modern MCP first.** The bridge answers `server/discover` and declares
  `2026-07-28`, the revision that removed the `initialize` handshake in favour of
  per-request `_meta`. It still answers legacy clients, because coexistence is
  part of the contract.
- **The dangerous tools are the same tools.** `open_order`, `close_position` and
  `halt_trading` are callable from a model. That is why the transport supports a
  token and TLS, and why `halt_trading` exists as a single-call kill switch.

## 60-second screencast script

| Time | Shot |
|------|------|
| 0–5 s | Terminal: `python3 tools/mcp_demo.py` starts running |
| 5–15 s | `server/discover` and `tools/list` output — 32 tools |
| 15–35 s | The four `tools/call` blocks scrolling with real payloads |
| 35–50 s | Cut to the Qt client, connected to the same socket, showing the same numbers |
| 50–60 s | `halt_trading` in the REPL, and the client's HALTED badge turning red |

Record at 1080p, keep the terminal font large, and do not narrate over the
`tools/list` block — let the tool count land on its own.

## Connecting a real client

With a live engine running:

```bash
claude mcp add pulsetrader -- /absolute/path/to/run.sh mcp
```

`run.sh mcp` loads `trading.toml` for the host, port and optional token. The MCP
mode keeps stdout clean for protocol traffic — console logging is forced to file
for that reason.

## Caveats

- The demo fixture serves **fixed sample data**. It is not a simulator.
- The AI parameter-tuning loop is experimental and unverified; see
  [Project status](../README.md#project-status).
- `open_order` against a live engine spends real money. Use testnet
  (`PULSE_NETWORK=testnet`) first.
