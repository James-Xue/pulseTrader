#!/usr/bin/env python3
"""Build a standalone BTC kline database, isolated from the live engine DB.

Why this exists
---------------
Research and replay need BTC 1m/5m/15m/1h/4h in one place. Today they live in
two uncomfortable forms:

  - data/binance/btc_{perp,spot}_{interval}.csv — complete history, but 11 M
    rows across ten CSVs that every tool has to re-scan;
  - data/trades.db `kline_bars` — the live engine's table. It is 698 MB,
    shared with every other symbol, held open and written by the running
    engine, and its primary key is (symbol, market_type, open_time) with NO
    interval column: a 1m bar and a 1h bar with the same open_time collide,
    so it cannot hold more than one interval per symbol.

This tool writes a separate `btc_klines.db` whose primary key includes the
interval and the source, leaving trades.db untouched:

    PRIMARY KEY (source, symbol, market_type, interval, open_time)

Sources
-------
  source='binance'  data/binance/btc_{perp,spot}_{iv}.csv (2019-09 -> latest)
  source='gate'     trades.db kline_bars for BTC_USDT, 1m only (execution
                    aligned; the engine records it live)

Usage
-----
    python3 tools/build_btc_db.py --out data/btc_klines.db
    python3 tools/build_btc_db.py --markets perp --intervals 1m,1h
    python3 tools/build_btc_db.py --out data/btc_klines.db --verify

The output lands in data/ (gitignored). Rebuild it whenever the CSV mirror is
updated; it is a derived artifact, never a source of truth.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import os
import sqlite3
import sys
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS klines (
    source      TEXT    NOT NULL,   -- binance | gate
    symbol      TEXT    NOT NULL,   -- Gate-style: BTC_USDT
    market_type TEXT    NOT NULL,   -- perp | spot (binance) / futures | spot (gate)
    interval    TEXT    NOT NULL,   -- 1m | 5m | 15m | 1h | 4h
    open_time   INTEGER NOT NULL,   -- epoch milliseconds
    open        REAL    NOT NULL,
    high        REAL    NOT NULL,
    low         REAL    NOT NULL,
    close       REAL    NOT NULL,
    volume      REAL    NOT NULL,
    PRIMARY KEY (source, symbol, market_type, interval, open_time)
) WITHOUT ROWID;
"""

INTERVALS = ("1m", "5m", "15m", "1h", "4h")
BATCH = 20_000


def _epoch(ts_ms: int) -> str:
    return datetime.datetime.fromtimestamp(
        ts_ms / 1000, datetime.UTC
    ).strftime("%Y-%m-%d %H:%M")


def open_target(path: Path, force: bool) -> sqlite3.Connection:
    if path.exists() and not force:
        raise SystemExit(f"{path} exists; pass --force to rebuild it")
    part = path.with_suffix(path.suffix + ".part")
    part.unlink(missing_ok=True)
    db = sqlite3.connect(part)
    # Bulk build: durability is irrelevant until the atomic rename below.
    db.execute("PRAGMA journal_mode=MEMORY")
    db.execute("PRAGMA synchronous=OFF")
    db.executescript(SCHEMA)
    return db


def import_csv(db: sqlite3.Connection, path: Path, market: str, interval: str) -> int:
    """Stream one mirror CSV into the table. Returns rows inserted."""
    if not path.exists():
        print(f"  skip (missing): {path}", file=sys.stderr)
        return 0
    rows: list[tuple] = []
    inserted = 0
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.reader(handle)
        next(reader, None)  # header
        for line in reader:
            try:
                ts = int(line[0])
                open_p, high, low, close, volume = (
                    float(line[1]),
                    float(line[2]),
                    float(line[3]),
                    float(line[4]),
                    float(line[5]),
                )
            except (IndexError, TypeError, ValueError):
                continue
            rows.append(
                ("binance", "BTC_USDT", market, interval, ts * 1000,
                 open_p, high, low, close, volume)
            )
            if len(rows) >= BATCH:
                _flush(db, rows)
                inserted += len(rows)
                rows.clear()
    if rows:
        _flush(db, rows)
        inserted += len(rows)
    return inserted


def import_gate(db: sqlite3.Connection, trades_db: Path) -> int:
    """Copy BTC_USDT 1m rows out of the live engine DB (read-only)."""
    if not trades_db.exists():
        print(f"  skip (missing): {trades_db}", file=sys.stderr)
        return 0
    source = sqlite3.connect(f"file:{trades_db}?mode=ro", uri=True)
    try:
        cursor = source.execute(
            "SELECT market_type, open_time, open, high, low, close, volume "
            "FROM kline_bars WHERE symbol = 'BTC_USDT' ORDER BY open_time"
        )
        rows: list[tuple] = []
        inserted = 0
        for market_type, open_time, open_p, high, low, close, volume in cursor:
            if market_type not in ("futures", "spot"):
                continue
            rows.append(
                ("gate", "BTC_USDT", market_type, "1m", int(open_time),
                 float(open_p), float(high), float(low), float(close),
                 float(volume))
            )
            if len(rows) >= BATCH:
                inserted += _flush(db, rows)
                rows.clear()
        if rows:
            inserted += _flush(db, rows)
        return inserted
    finally:
        source.close()


def _flush(db: sqlite3.Connection, rows: list[tuple]) -> int:
    before = db.total_changes
    db.executemany(
        "INSERT OR REPLACE INTO klines "
        "(source, symbol, market_type, interval, open_time, open, high, low, "
        " close, volume) VALUES (?,?,?,?,?,?,?,?,?,?)",
        rows,
    )
    return db.total_changes - before


def build(args: argparse.Namespace) -> int:
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    db = open_target(out, args.force)
    csv_dir = Path(args.csv_dir)
    markets = [m.strip() for m in args.markets.split(",") if m.strip()]
    intervals = [i.strip() for i in args.intervals.split(",") if i.strip()]

    total = 0
    try:
        for market in markets:
            for interval in intervals:
                path = csv_dir / f"btc_{market}_{interval}.csv"
                added = import_csv(db, path, market, interval)
                total += added
                print(f"  binance {market:4} {interval:3}: {added:>9} rows")
        if not args.no_gate:
            added = import_gate(db, Path(args.trades_db))
            total += added
            print(f"  gate    futures 1m : {added:>9} rows (BTC_USDT)")
        db.commit()
    finally:
        db.close()

    part = out.with_suffix(out.suffix + ".part")
    os.replace(part, out)
    size_mb = out.stat().st_size / (1024 * 1024)
    print(f"\nwrote {out} — {total} rows, {size_mb:.0f} MB")
    return 0


def verify(path: Path) -> int:
    if not path.exists():
        raise SystemExit(f"{path} not found")
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    expected_ms = {"1m": 60_000, "5m": 300_000, "15m": 900_000,
                   "1h": 3_600_000, "4h": 14_400_000}
    print(f"{'source':8} {'market':8} {'iv':3} {'rows':>10}  {'first':16}  {'last':16}  gaps")
    try:
        groups = db.execute(
            "SELECT source, market_type, interval, COUNT(*), MIN(open_time), "
            "MAX(open_time) FROM klines GROUP BY 1,2,3 ORDER BY 1,2,3"
        ).fetchall()
        for source, market, interval, count, first, last in groups:
            step = expected_ms[interval]
            gaps = 0
            prev = None
            for (ts,) in db.execute(
                "SELECT open_time FROM klines WHERE source=? AND market_type=? "
                "AND interval=? ORDER BY open_time",
                (source, market, interval),
            ):
                if prev is not None and ts - prev != step:
                    gaps += 1
                prev = ts
            print(f"{source:8} {market:8} {interval:3} {count:>10}  "
                  f"{_epoch(first):16}  {_epoch(last):16}  {gaps}")
    finally:
        db.close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="data/btc_klines.db")
    parser.add_argument("--csv-dir", default="data/binance")
    parser.add_argument("--trades-db", default="data/trades.db")
    parser.add_argument("--markets", default="perp,spot")
    parser.add_argument("--intervals", default="1m,5m,15m,1h,4h")
    parser.add_argument("--no-gate", action="store_true",
                        help="skip the engine-recorded BTC_USDT 1m rows")
    parser.add_argument("--force", action="store_true",
                        help="rebuild even if the target exists")
    parser.add_argument("--verify", action="store_true",
                        help="print coverage/gap stats for an existing DB")
    args = parser.parse_args()
    if args.verify:
        return verify(Path(args.out))
    return build(args)


if __name__ == "__main__":
    sys.exit(main())
