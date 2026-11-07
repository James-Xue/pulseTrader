#!/usr/bin/env python3
"""fetch_funding_history.py — fetch Gate USDT-M perpetual funding-rate history (2026-09-06)

Gate v4 `/futures/usdt/funding_rate` is a public endpoint, but the server only keeps
the most recent **180 days** (a `from` earlier than now-180d returns 400 "from time
exceeds 180-day limit"). It returns [{r, t}] in reverse chronological order, where
r = rate (a string; 0.00005 = 5bps/8h).

Usage:
    python3 tools/fetch_funding_history.py [--symbols BTC_USDT ETH_USDT ...]
    python3 tools/fetch_funding_history.py --symbols ALL   # fetch the 4 mainnet futures

Output: data/funding/gate_<contract>.csv (ts_iso,t_sec,rate)
Idempotent: an existing file skips the t values already covered (deduplicated by t
and resumed).
"""

import argparse
import csv
import datetime as dt
import json
import sys
import time
import urllib.parse
import urllib.request

BASE = "https://api.gateio.ws/api/v4/futures/usdt/funding_rate"
RETENTION_DAYS = 180  # Gate server-side hard limit (measured 2026-09-06)
PAGE = 100

DEFAULT_SYMBOLS = ["BTC_USDT", "ETH_USDT", "SNDK_USDT", "UNITREE_USDT"]


def fetch(symbol: str, to_sec: int, limit: int) -> list:
    q = urllib.parse.urlencode(
        {"contract": symbol, "to": to_sec, "limit": limit})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(
                    f"{BASE}?{q}", timeout=30) as resp:
                return json.load(resp)
        except urllib.error.HTTPError as e:
            if e.code == 429:
                time.sleep(1.5 * (attempt + 1))
                continue
            print(f"  HTTP {e.code}: {e.read().decode()[:200]}", file=sys.stderr)
            return None
        except Exception as e:  # Network jitter → back off and retry
            time.sleep(1.0 * (attempt + 1))
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Gate perpetual funding-rate history (≤180d)")
    ap.add_argument("--symbols", nargs="*", default=None,
                    help="contract list; defaults to the 4 mainnet futures; ALL = the 4 mainnet futures")
    ap.add_argument("--out", default="data/funding")
    ap.add_argument("--rate-sec", type=float, default=0.4,
                    help="seconds between requests (default 0.4)")
    args = ap.parse_args()
    symbols = args.symbols
    if not symbols or symbols == ["ALL"]:
        symbols = DEFAULT_SYMBOLS

    now = int(time.time())
    start = now - RETENTION_DAYS * 86400
    import os
    os.makedirs(args.out, exist_ok=True)

    for sym in symbols:
        path = f"{args.out}/gate_{sym}.csv"
        seen = set()
        if os.path.exists(path):
            with open(path) as fh:
                for row in csv.DictReader(fh):
                    seen.add(int(row["t_sec"]))
        rows, to, exhausted = {}, now, False
        pages = 0
        while not exhausted:
            batch = fetch(sym, to, PAGE)
            if batch is None:
                print(f"{sym}: request failed, aborting", file=sys.stderr)
                return 1
            pages += 1
            if not batch:
                exhausted = True
                break
            for rec in batch:
                t, r = int(rec["t"]), float(rec["r"])
                if t >= start:
                    rows[t] = r
                if t < start:
                    exhausted = True
            newest = min(rr["t"] for rr in batch)
            if to - newest <= 1:  # No progress → guard against an infinite loop
                break
            to = newest - 1
            time.sleep(args.rate_sec)
        fresh = {t: r for t, r in rows.items() if t not in seen}
        if fresh:
            existed = len(seen)
            with open(path, "a", newline="") as fh:
                w = csv.writer(fh)
                if existed == 0:
                    w.writerow(["ts_iso", "t_sec", "rate"])
                for t in sorted(fresh):
                    iso = dt.datetime.fromtimestamp(
                        t, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
                    w.writerow([iso, t, f"{fresh[t]:.8f}"])
            span = (max(rows) - min(rows)) / 86400 if rows else 0
            print(f"{sym}: {len(fresh)} new rows "
                  f"({pages} pages, covering {span:.0f} days, "
                  f"{dt.datetime.fromtimestamp(min(rows),dt.timezone.utc).strftime('%m-%d')}→"
                  f"{dt.datetime.fromtimestamp(max(rows),dt.timezone.utc).strftime('%m-%d')})")
        else:
            print(f"{sym}: already up to date ({len(seen)} rows, skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
