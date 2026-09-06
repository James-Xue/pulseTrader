#!/usr/bin/env python3
"""fetch_funding_history.py — Gate USDT-M 永续资金费率历史拉取 (2026-09-06)

Gate v4 `/futures/usdt/funding_rate` 是公开端点,但服务端只保留最近
**180 天**(from 早于 now-180d 报 400 "from time exceeds 180-day limit")。
返回按时间倒序的 [{r, t}],r = 费率(字符串,0.00005 = 5bps/8h)。

用法:
    python3 tools/fetch_funding_history.py [--symbols BTC_USDT ETH_USDT ...]
    python3 tools/fetch_funding_history.py --symbols ALL   # 拉主网4期货

落盘:data/funding/gate_<contract>.csv(ts_iso,t_sec,rate)
幂等:已存在文件则跳过已覆盖的 t(按 t 去重续拉)。
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
RETENTION_DAYS = 180  # Gate 服务端硬上限(实测 2026-09-06)
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
        except Exception as e:  # 网络抖动 → 退避重试
            time.sleep(1.0 * (attempt + 1))
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Gate 永续资金费率历史 (≤180d)")
    ap.add_argument("--symbols", nargs="*", default=None,
                    help="合约列表;缺省主网4期货;ALL = 主网4期货")
    ap.add_argument("--out", default="data/funding")
    ap.add_argument("--rate-sec", type=float, default=0.4,
                    help="请求间隔秒(默认 0.4)")
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
                print(f"{sym}: 请求失败,中止", file=sys.stderr)
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
            if to - newest <= 1:  # 没前进 → 防死循环
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
            print(f"{sym}: 新增 {len(fresh)} 条 "
                  f"({pages} 页,覆盖 {span:.0f} 天,"
                  f"{dt.datetime.fromtimestamp(min(rows),dt.timezone.utc).strftime('%m-%d')}→"
                  f"{dt.datetime.fromtimestamp(max(rows),dt.timezone.utc).strftime('%m-%d')})")
        else:
            print(f"{sym}: 已是最新({len(seen)} 条,跳过)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
