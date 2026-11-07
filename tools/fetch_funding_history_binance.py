#!/usr/bin/env python3
"""fetch_funding_history_binance.py — full funding-rate history for Binance USDⓈ-M perpetuals (2026-09-06)

Binance /fapi/v1/fundingRate has no 180-day cap and can go back to contract
listing (2019-09-10). Public endpoint; a direct connection from China is
unstable → use the environment HTTPS_PROXY (Clash 7897 JP egress, the same
tunnel as the data/binance mirror). Pagination: startTime ascending pages,
limit=1000 per page.

Usage:
    python3 tools/fetch_funding_history_binance.py            # BTCUSDT ETHUSDT
    python3 tools/fetch_funding_history_binance.py --symbols BTCUSDT,SNDKUSDT

Output: data/funding/binance_<SYMBOL>_funding.csv (ts_iso,t_ms,rate)
Idempotent: deduplicates by t_ms and resumes (an existing file skips the ranges
already covered).
"""

import argparse
import csv
import datetime as dt
import json
import os
import sys
import time
import urllib.parse
import urllib.request

BASE = "https://fapi.binance.com/fapi/v1/fundingRate"
PAGE = 1000
DEFAULT_SYMBOLS = ["BTCUSDT", "ETHUSDT"]  # Binance-style symbols have no underscore


def get(params):
    url = f"{BASE}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(
        url, headers={"User-Agent": "pulseTrader/1.0"})
    with urllib.request.urlopen(req, timeout=40) as r:
        return json.load(r)


def main() -> int:
    ap = argparse.ArgumentParser(description="Binance perpetual funding-rate full history")
    ap.add_argument("--symbols", default=",".join(DEFAULT_SYMBOLS),
                    help="Binance-style symbols, comma-separated (default BTCUSDT,ETHUSDT)")
    ap.add_argument("--out", default="data/funding")
    ap.add_argument("--rate-sec", type=float, default=0.6,
                    help="seconds between requests (default 0.6; do not overload the mirror channel)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    for sym in [s.strip() for s in args.symbols.split(",") if s.strip()]:
        path = f"{args.out}/binance_{sym}_funding.csv"
        seen = set()
        if os.path.exists(path):
            with open(path) as fh:
                for row in csv.DictReader(fh):
                    seen.add(int(row["t_ms"]))

        rows, cursor, pages = {}, 1, 0  # startTime=1 → page the full history (startTime=0 is treated as omitted and only returns the latest 500)
        while True:
            try:
                batch = get({"symbol": sym, "startTime": cursor, "limit": PAGE})
            except urllib.error.HTTPError as e:
                print(f"{sym}: HTTP {e.code} {e.read().decode()[:160]}",
                      file=sys.stderr)
                return 1
            except Exception as e:
                print(f"{sym}: network error {e} (retrying)", file=sys.stderr)
                time.sleep(2)
                continue
            pages += 1
            if not batch:
                break
            for rec in batch:
                rows[int(rec["fundingTime"])] = float(rec["fundingRate"])
            last_t = batch[-1]["fundingTime"]
            if len(batch) < PAGE:  # on the last page
                break
            cursor = last_t + 1
            time.sleep(args.rate_sec)

        fresh = {t: r for t, r in rows.items() if t not in seen}
        if fresh:
            with open(path, "a", newline="") as fh:
                w = csv.writer(fh)
                if not seen:
                    w.writerow(["ts_iso", "t_ms", "rate"])
                for t in sorted(fresh):
                    iso = dt.datetime.fromtimestamp(
                        t / 1000, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
                    w.writerow([iso, t, f"{fresh[t]:.8f}"])
            yrs = (max(rows) - min(rows)) / 86400e3 / 365.25
            print(f"{sym}: {len(fresh)} new / {len(seen)+len(fresh)} total "
                  f"({pages} pages, {yrs:.1f} years, "
                  f"{dt.datetime.fromtimestamp(min(rows)/1000,dt.timezone.utc).strftime('%Y-%m-%d')} → "
                  f"{dt.datetime.fromtimestamp(max(rows)/1000,dt.timezone.utc).strftime('%Y-%m-%d')})")
        else:
            print(f"{sym}: already up to date ({len(seen)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
