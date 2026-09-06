#!/usr/bin/env python3
"""calibrate_fill_model.py — 用 trades.db 真实成交标定纸面填充参数

从真实执行记录统计(按 symbol×market_type 分组,样本 <30 自动并入
同 market_type 池):
  - post_only(maker-first)成交率 与 撤单率 → 校验 tick 驱动填充行为
  - slippage_bps 分位数(P50/P90)按 order_type → 纸面模型误差带
  - taker/market 兜底占比 → maker_timeout 合理窗口
  - 出场侧 order_type 分布 → 平仓费模型核对

用法:
  python3 tools/trial/calibrate_fill_model.py [--db data/trades.db]
      [--out data/trial/fill_model.json] [--since-ns 0] [--top 15]
--out 缺省只打印不写文件。trades 表只含"最终成交/撤单"行,
数据库无独立撤单事件时按 final_status 统计。
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
    p.add_argument("--out", help="写 JSON 标定文件(缺省只打印)")
    p.add_argument("--since-ns", type=int, default=0)
    p.add_argument("--top", type=int, default=15)
    args = p.parse_args()

    if not os.path.exists(args.db):
        print(f"DB 不存在: {args.db}", file=sys.stderr)
        return 2

    con = sqlite3.connect(f"file:{args.db}?mode=ro", uri=True)
    # 按 (symbol, market_type) 分组统计
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
        # 引擎把 maker-first 单落库为 order_type='limit'(M16);post_only
        # 与 limit 同入 maker 桶;market 独立;其余入 other
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
        print("0 行(该时间窗无成交)。")
        return 1

    # 单 symbol 组样本太少 → 标记 pooled(统计并入同 market_type 池,
    # 供横向参考),但仍在表内列出实际数字
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

    print(f"共 {n_rows} 行成交/撤单记录\n")
    hdr = ("symbol       mtype     mk单  mk成交率  mk市价兜底   "
           "滑点bps maker(p50/p90/n)    market(p50/p90/n)")
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
        print(f"\n标定写入 {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
