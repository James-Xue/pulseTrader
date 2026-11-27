# Paper Trading — ledger, capital events and reconciliation

> Paper mode is a separate engine process (`trading.paper.toml`) with its own
> lock, control port, state file, trade database and logs. It never places a
> real order.

## Runtime model

- The **paper venue** (`PaperVenue`) is the exchange: it owns positions, fills
  market orders on the next poll, charges the fee model, and exposes
  venue-initiated exits as data (`PaperExitEvent`) for Layer 9 to record.
- The **engine book** (`PositionManager`) mirrors the venue. Since the
  2026-11-27 reconciliation audit, CFD is **1:1 with the venue**: an entry
  always opens its own position and never offsets another member's position.
- The **trade recorder** (`trades-paper.db`) is the append-only audit: one row
  per entry and one PnL-bearing row per venue exit, carrying the venue's
  realized PnL and fee.
- **Capital events** (`PaperAccount`) record every seed / top-up / withdrawal /
  reset, so the account identity is checkable.

## Account identity

Per wallet leg:

```
cash + locked_margin == deposits + realized_pnl - fees_paid
```

`deposits` is the configured seed until a capital event changes it. A reset
opens a new epoch: cash becomes the new deposit and the leg's realized/fee
counters restart, so the identity holds from the new epoch onward.

At startup the engine logs any residual above **0.005 USDT as a WARN**. It
never refuses to start: a hand-edited historical state file legitimately shows
a gap, and locking the operator out of the ledger that needs repairing would
be worse.

## `paper_capital` (control socket / MCP)

```json
{"leg": "cfd", "kind": "topup", "amount": 25.0, "note": "operator top-up"}
```

| kind | effect |
|---|---|
| `seed` | sets cash and deposits to `amount` |
| `topup` | adds `amount` to cash and deposits |
| `withdraw` | subtracts `amount` from cash and deposits |
| `reset` | **flat book required**; sets cash/deposits to `amount` and starts a new epoch |

The method returns the recorded event, the account snapshot and the identity
residual. Every event is also appended to `<state_file>.capital.jsonl`.
A reset is refused while any venue or engine position is open, and refused by
the account itself while margin is locked.

## Legacy reconciliation adjustments

The four historical epochs (E0–E3) predate capital events. Startup appends
four **legacy_adjustment** events exactly once (idempotent by id):

| id | realized delta | fee delta |
|---|---|---|
| `legacy:E0` | −5.91 | 0.00 |
| `legacy:E1` | −10.07 | −0.54 |
| `legacy:E2` | 0.00 | −0.06 |
| `legacy:E3` | 0.00 | 0.00 |

They move no cash; they document the gap between the engine's trade ledger and
the account snapshot for the audit trail.

## Crash window

The state file is an atomic snapshot (`tmp + fsync + rename`, previous copy
kept as `.bak`). A `kill -9` between a venue close and the next snapshot is
safe to replay: the restarted engine restores the venue position and mints the
same `paper_exit_<venue id>_<seq>` order id, which the recorder rejects as a
duplicate (`order_id` is UNIQUE). The trade is never double-counted.

## Reconciliation evidence

`tests/fixtures/reconciliation/` holds the read-only evidence used by the
reconciliation suite (four account snapshots, the trades-only DB copy, the E1
replay legs). See `docs/audits/paper-team-reconciliation-2026-11-27.md` and
the fixtures README before changing any of it.
