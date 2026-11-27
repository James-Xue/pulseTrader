# Reconciliation fixtures — 2026-11-27 paper-team audit (P0)

Frozen, read-only evidence for the audit at
`docs/audits/paper-team-reconciliation-2026-11-27.md`. These files are the input to
`tests/unit/control/test_reconciliation_contract.cpp` (P0 canaries) and will be the
input to the P1/P2 red-first tests.

**Do not regenerate these from `data/`.** The live paper engine keeps writing
`data/trades-paper.db` and its `-wal`, so a copy taken later is a different dataset.
Replacing a fixture invalidates every number in audit §2 and should be treated as a
deliberate change requiring a new audit entry.

## Contents

| File | Size | SHA-256 |
|---|---|---|
| `trades-paper-epochs.db` | 90 KB | `738a9fd972cc1ca532032efb38f80e1f98056ed27dba9c2b4354819fd1f21ba3` |
| `paper-state/paper-state.json` | 1057 B | `d9cc73aa5e4e066afae58a48605a059792e0f8609464adc124efec6d4be8fa5d` |
| `paper-state/paper-state.json.bak-20261126-100usd` | 674 B | `2be5114ef96db52192842d4e761314c414cbe31cabf8b06f404d5cc5259617a0` |
| `paper-state/paper-state.json.bak-20261127-pre-topup` | 648 B | `497f5a3dfd54a17820864dbbb7906d3b7d30f1424a7bffad49dbea34df212ead` |
| `paper-state/paper-state.json.bak-20261127-pre-final-reset` | 650 B | `77a07eb2b2ac6b31e2d88d8597be7d140032c7330856ce76d0bf484d0e8a3571` |
| `paper-state/paper-state.json.bak-20261127-post-trade` | 650 B | `77a07eb2b2ac6b31e2d88d8597be7d140032c7330856ce76d0bf484d0e8a3571` |

`paper-state.json.bak-20261127-post-trade` is byte-identical to the pre-final-reset
copy; both are kept because they mark distinct audit events (the 09:46 reset and the
09:45:53 trade that preceded it).

## Provenance

All files were copied from the audit's read-only snapshot `/tmp/pulse-audit/` taken at
**2026-11-27 12:53 CST** (audit §8). That snapshot was itself produced by SQLite's
`.backup` against the live engine database, so it is a consistent copy, not a raw WAL
read.

| Fixture | Audit role |
|---|---|
| `trades-paper-epochs.db` | E0–E3 trade rows: 210 rows, ids 1–211 with **id 64 absent**, Σpnl −99.22, Σfees 6.18 |
| `paper-state.json` | E3 snapshot at 12:53 (cash 88.2425 + locked 3.6375) |
| `...bak-20261126-100usd` | E0 snapshot (pre-first-reset) |
| `...bak-20261127-pre-topup` | E1 snapshot (pre-second-reset) |
| `...bak-20261127-pre-final-reset` | E2 snapshot |
| `...bak-20261127-post-trade` | E2 evidence after the 09:45:53 trade |

### Why the trade database is trimmed

The source copy is 39 MB, almost entirely `ticker_ticks` (28 MB + 9.8 MB index).
Only the `trades` table carries reconciliation evidence, so the fixture keeps exactly
that table (and its indexes), and drops `ticker_ticks`, `kline_bars` and `signals`.

```bash
# Reproduce the trim from a fresh `.backup` copy (never from the live file):
sqlite3 /tmp/pulse-audit/trades-paper.db \
        "VACUUM INTO 'tests/fixtures/reconciliation/trades-paper-epochs.db'"
sqlite3 tests/fixtures/reconciliation/trades-paper-epochs.db \
        "DROP TABLE ticker_ticks; DROP TABLE kline_bars; DROP TABLE signals; VACUUM;"
```

The trim preserves the `trades` rows byte-for-byte, including `sqlite_sequence`
(`trades` → 211) and `PRAGMA user_version` = 1, which is the pre-migration schema the
audit read. P1/P5 must migrate the **live** schema to v2; this fixture stays at v1 so
"old binary still reads v1" remains testable.

## Rules for tests

1. Open fixtures with `SQLITE_OPEN_READONLY` / `mode=ro`. Never `immutable=1`.
2. Never write to a fixture, and never let a test create `-wal`/`-shm` beside it
   (`FixtureStaysReadOnly` asserts this).
3. Classify rows by `trades.timestamp` through the frozen helper in
   `tests/unit/control/reconciliation_epochs.hpp`, not by id. The two agree today;
   the helper is the contract.
4. Units are UNIX **nanoseconds** for `trades.timestamp`. JSON `time_ms` is
   milliseconds and is converted at the call site.

## Deliberately absent

- The 13:39–13:49 "live" observation in audit §2 (equity 92.19): it was never
  captured as a file, and a fixture must be re-readable byte-for-byte.
- The GT ledger: it lives in the companion repository at
  `11_gold-trader-mind/tests/fixtures/reconciliation/gt-ledger-current.db`, because
  the P3 tests run there.
- A synthetic two-member ledger: it is built in code by the P3 member-scoped rebuild
  tests rather than committed as a binary.
