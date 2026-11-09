# Decision intake — the bridge from the decision repository

`submit_decision` is the only path by which an outside decision system (the
`gold-trader-mind` repository) can talk to the engine. It exists so the two
halves have a written contract instead of an informal one.

**It validates and reports. It never places, cancels or modifies an order.**

## Why it is dry-run only

A decision packet arrives from outside this process, produced by code this
engine does not control. Trusting it to size a position would put the entire
risk model on the other side of a boundary. So the engine:

1. validates the packet,
2. recomputes the risk arithmetic from the stop distance and the contract size,
3. returns what it *would* do, with `would_execute: false`.

Live execution is a separate, later step that must add its own tests and its
own account-aware checks. Until then, an operator acts on the verdict, or the
existing `open_order` path is used.

## Contract

Schema `gold.decision/1`. The authoritative definition lives beside the
decision documents (`gold-trader-mind/15_system-boundary.md`); this file only
describes how the engine treats it.

| Field | Rule enforced here |
|---|---|
| `schema` | must equal `gold.decision/1` |
| `id` | non-empty string; echoed back for attribution |
| `valid_until` | `YYYY-MM-DDTHH:MM:SSZ`; expired packets are discarded, never caught up |
| `instrument.venue` | must be `gate-tradfi` (venue fixed 2026-11-09) |
| `instrument.contract` | must be `cfd` |
| `stance` | `long` / `short` / `flat` / `abstain` |
| `levels.entry_zone` | `[low, high]`, `0 < low <= high` |
| `levels.stop` | positive; below the zone for longs, above it for shorts |
| `levels.targets` | non-empty array |
| `sizing.risk_R` | `> 0` and `<= 1` (matches the 1R rule) |
| `sizing.stop_distance` | positive; a >1% disagreement with the levels raises a warning |

`stance: "abstain"` is a **successful** validation with `accepted: false` — a
decision to do nothing is a decision, not a malformed packet.

## Verdict

```json
{
  "id": "2026-11-09T14:05:00Z-xauusd-01",
  "accepted": true,
  "dry_run": true,
  "would_execute": false,
  "stance": "long",
  "computed": {
    "risk_per_lot_usd": 850.0,
    "ounces_per_lot": 100.0,
    "risk_R": 1.0,
    "entry_reference": 4025.0,
    "note": "lots are NOT trusted; a live path must derive them from account equity"
  },
  "warnings": [],
  "reason": "validated; dry run only — no order was placed"
}
```

Malformed or unsafe packets return a `PulseError` with
`ControlInvalidRequest` (9101). Expired packets and `abstain` return a normal
verdict with `accepted: false` and a `reason`.

## Calling it

Over the control socket (also available as the MCP tool of the same name):

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"submit_decision","params":{
  "schema":"gold.decision/1","id":"demo-1","valid_until":"2099-01-01T00:00:00Z",
  "instrument":{"symbol":"XAUUSD","venue":"gate-tradfi","contract":"cfd"},
  "stance":"long",
  "levels":{"entry_zone":[4021.5,4025.0],"stop":4012.0,"targets":[4038.0]},
  "sizing":{"risk_R":1.0,"stop_distance":8.5}}}' | nc 127.0.0.1 8081
```

## What must change before this can trade

1. Derive lots from live equity and the configured single-trade risk budget —
   not from the packet.
2. Run the full risk stack (daily/weekly/monthly budgets, notional caps,
   drawdown guard, event windows) before submission.
3. Write an audit record keyed by `id` so fills can be attributed back to the
   packet that produced them.
4. Add tests for each of those, exactly as the validation path has.
