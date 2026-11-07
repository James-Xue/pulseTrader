# Launch drafts

Copy-paste material for the first public push. Nothing here is posted
automatically — publishing is a human decision, on your own accounts.

---

## Show HN

### Title candidates (pick one, keep it factual)

1. `Show HN: A C++20 trading engine you can drive over MCP`
2. `Show HN: pulseTrader – a Gate.io trading engine with an MCP control plane`
3. `Show HN: I built a trading engine, then could not prove my strategies work`

Option 3 is the strongest for Hacker News because the negative result is the
unusual part, and it is true: a 480-combination sweep on 1-minute BTC found no
robust positive expectancy.

### Body

```
I built a C++20 trading engine for Gate.io over the last year. The interesting
part is not that it trades — it is that I could not show it makes money, and
that the engine is honest about it.

What is real:
- Nine layers with strict boundaries; the market-data hot path never blocks on
  AI, disk or the network. 1054 automated tests.
- A control plane: newline-delimited JSON-RPC over TCP, 32 methods whose names
  double as MCP tool names, so Claude Desktop / Claude Code can read status,
  positions and orders and place manual orders.
- Optional TLS + static-token auth on that socket (off by default).
- A Qt 6.12 desktop client, read-only for now.

What is not:
- The AI tuning loop (LLM every 5 minutes) is experimental and unverified. It
  has never run in production.
- No strategy has a robust positive expectancy. I swept 480 parameter
  combinations on 1-minute BTC and documented that it does not work.
- There is no paper-trading mode yet, and the backtester ignores funding,
  margin, liquidation and slippage.

Trying it takes no keys and no build:
    docker build -f docker/demo.Dockerfile -t pulsetrader-demo .
    docker run --rm -p 8081:8081 pulsetrader-demo
    printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"get_status"}' | nc 127.0.0.1 8081

I would especially like feedback on the control-plane protocol shape and on
what a credible paper-trading mode should model.

https://github.com/James-Xue/pulseTrader
```

### Timing and etiquette

- Post on a weekday morning US Eastern time; avoid Friday afternoon.
- Reply to every comment in the first two hours.
- One submission only. If it does not move, do not re-post the same link.

---

## awesome-mcp-servers entry

Most MCP lists accept a PR that adds one line to a table or a bullet list.

### Entry

```
- [pulseTrader](https://github.com/James-Xue/pulseTrader) — C++20 trading engine for Gate.io exposing 32 control-plane methods as MCP tools (status, positions, orders, strategies, risk). Read-only client included; TLS and token auth optional.
```

### PR description

```
Adds pulseTrader, a C++20 trading engine whose control plane speaks
newline-delimited JSON-RPC and exposes the same 32 method names as MCP tools.
The MCP bridge ships as `./run.sh mcp` (stdio) and needs no API key to try:
`tools/demo_engine.py` serves the wire contract with sample payloads.

The AI tuning loop in the project is labelled experimental and unverified, and
the README states plainly that no strategy has a proven edge.
```

### Before submitting

- Read the target list's contribution rules; several require alphabetical order
  or a specific table format.
- Only submit to lists that accept third-party additions by PR. Do not open PRs
  on repositories whose maintainers have asked for issues first.
