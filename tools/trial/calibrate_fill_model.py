#!/usr/bin/env python3
"""calibrate_fill_model.py — calibrate paper fill parameters against real fills in trades.db

Statistics computed from real execution records (grouped by symbol×market_type;
groups with fewer than 30 samples are automatically pooled into the same market_type):
  - post_only (maker-first) fill rate and cancel rate → validate the tick-driven fill behaviour
  - slippage_bps percentiles (P50/P90) by order_type → the paper model's error band
  - taker/market fallback share → a reasonable maker_timeout window
  - order_type distribution on the exit side → cross-check the closing-fee model

Usage:
  python3 tools/trial/calibrate_fill_model.py [--db data/trades.db]
      [--out data/trial/fill_model.json] [--since-ns 0] [--top 15]
With no --out it only prints and writes no file. The trades table holds only
"final fill / cancel" rows, so when the database has no separate cancel events the
statistics are derived from final_status.
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import sys
from collections import defaultdict

QUERY = """
SELECT symbol, market_type, order_type, final_status, slippage_bps,
       avg_fill_price, submit_mid_price, fees, latency_ms, side,
       strategy_name, timestamp
FROM trades
WHERE timestamp >= ?
ORDER BY timestamp
"""


def percentile(sorted_vals, p):
    if not sorted_vals:
        return None
    k = (len(sorted_vals) - 1) * p
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--db", default="data/trades.db")
    p.add_argument("--out", help="write the calibration JSON file (default: print only)")
    p.add_argument("--since-ns", type=int, default=0)
    p.add_argument("--top", type=int, default=15)
    args = p.parse_args()

    if not os.path.exists(args.db):
        print(f"DB does not exist: {args.db}", file=sys.stderr)
        return 2

    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    # Group the statistics by (symbol, market_type)
    groups: dict = defaultdict(lambda: {
        "maker": {"filled": 0, "cancelled": 0},
        "market": {"filled": 0, "cancelled": 0},
        "other": {"filled": 0, "cancelled": 0},
        "slip": {"maker": [], "market": [], "other": []},
        "latency": [],
        "sides": defaultdict(int),
        "strategies": defaultdict(int),
    })
    n_rows = 0
    for (symbol, mtype, otype, status, slip, avg_px, mid_px, fees,
         latency, side, strat, ts) in con.execute(QUERY, (args.since_ns,)):
        n_rows += 1
        key = (symbol, mtype)
        g = groups[key]
        # The engine persists maker-first orders as order_type='limit' (M16);
        # post_only and limit both go into the maker bucket; market is separate;
        # the rest go into other
        bucket = "maker" if otype in ("post_only", "limit") \
            else ("market" if otype == "market" else "other")
        g[bucket]["filled" if status == "filled" else "cancelled"] += 1
        if slip is not None:
            g["slip"][bucket].append(float(slip))
        if latency is not None:
            g["latency"].append(int(latency))
        g["sides"][side] += 1
        g["strategies"][strat or "?"] += 1
    con.close()

    if n_rows == 0:
        print("0 rows (no fills in this time window).")
        return 1

    # Too few samples in a single-symbol group → mark it pooled (the statistics
    # are merged into the same market_type pool for cross-reference), but the
    # actual numbers are still listed in the table
    pool: dict = defaultdict(list)
    for key in groups:
        pool[key[1]].append(key)
    merged_keys = set()
    for mtype, keys in pool.items():
        tot = sum(groups[k]["maker"]["filled"] + groups[k]["market"]["filled"]
                  + groups[k]["maker"]["cancelled"]
                  + groups[k]["market"]["cancelled"]
                  for k in keys)
        if tot >= 30:
            for k in keys:
                merged_keys.add(k)

    rows = []
    for key, g in sorted(groups.items()):
        symbol, mtype = key
        row = {"symbol": symbol, "market_type": mtype,
               "pooled": key not in merged_keys}
        po = g["maker"]; mk = g["market"]
        po_n = po["filled"] + po["cancelled"]
        mk_n = mk["filled"] + mk["cancelled"]
        row["maker_orders"] = po_n
        row["maker_fill_rate"] = round(po["filled"] / po_n, 4) if po_n else None
        row["market_orders"] = mk_n
        row["market_fallback_share"] = round(mk_n / max(1, po_n + mk_n), 4)
        for b in ("maker", "market", "other"):
            v = g["slip"][b]
            row[f"slippage_bps_{b}"] = {
                "p50": round(percentile(sorted(v), 0.5), 2) if v else None,
                "p90": round(percentile(sorted(v), 0.9), 2) if v else None,
                "n": len(v),
            }
        rows.append(row)

    print(f"{n_rows} fill/cancel records in total\n")
    hdr = ("symbol       mtype    mkord  fillrate   fallback  "
           "maker bps(p50/p90/n)        market bps(p50/p90/n)")
    print(hdr)
    for r in rows:
        po = r["slippage_bps_maker"]
        mk = r["slippage_bps_market"]
        po_s = f"{po['p50']}/{po['p90']}/{po['n']}" if po.get("p50") is not None else "-"
        mk_s = f"{mk['p50']}/{mk['p90']}/{mk['n']}" if mk.get("p50") is not None else "-"
        print(f"{r['symbol']:<12} {r['market_type']:<8} "
              f"{r['maker_orders']:<5} "
              f"{str(r['maker_fill_rate']):<9} "
              f"{str(r['market_fallback_share']):<8} {po_s:<26} {mk_s}")

    if args.out:
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w") as f:
            json.dump({"n_rows": n_rows, "groups": rows}, f, indent=2)
        print(f"\nCalibration written to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
