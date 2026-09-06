#!/usr/bin/env python3
"""fetch_funding_history_binance.py — 币安 USDⓈ-M 永续资金费率全史 (2026-09-06)

币安 /fapi/v1/fundingRate 无 180 天上限,可回溯到合约上市(2019-09-10)。
公开端点;本机直连中国不稳 → 走环境 HTTPS_PROXY(Clash 7897 JP 出口,
与 data/binance 镜像同隧道)。分页:startTime 升序翻页,limit=1000/页。

用法:
    python3 tools/fetch_funding_history_binance.py            # BTCUSDT ETHUSDT
    python3 tools/fetch_funding_history_binance.py --symbols BTCUSDT,SNDKUSDT

落盘:data/funding/binance_<SYMBOL>_funding.csv(ts_iso,t_ms,rate)
幂等:按 t_ms 去重续拉(已存在文件跳过已覆盖区间)。
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
DEFAULT_SYMBOLS = ["BTCUSDT", "ETHUSDT"]  # Binance 风格无下划线


def get(params):
    url = f"{BASE}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(
        url, headers={"User-Agent": "pulseTrader/1.0"})
    with urllib.request.urlopen(req, timeout=40) as r:
        return json.load(r)


def main() -> int:
    ap = argparse.ArgumentParser(description="币安永续资金费率全史")
    ap.add_argument("--symbols", default=",".join(DEFAULT_SYMBOLS),
                    help="Binance 风格符号,逗号分隔(默认 BTCUSDT,ETHUSDT)")
    ap.add_argument("--out", default="data/funding")
    ap.add_argument("--rate-sec", type=float, default=0.6,
                    help="请求间隔秒(默认 0.6,勿压镜像通道)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    for sym in [s.strip() for s in args.symbols.split(",") if s.strip()]:
        path = f"{args.out}/binance_{sym}_funding.csv"
        seen = set()
        if os.path.exists(path):
            with open(path) as fh:
                for row in csv.DictReader(fh):
                    seen.add(int(row["t_ms"]))

        rows, cursor, pages = {}, 1, 0  # startTime=1 → 全史翻页(startTime=0 会被当作未传,只回最近500)
        while True:
            try:
                batch = get({"symbol": sym, "startTime": cursor, "limit": PAGE})
            except urllib.error.HTTPError as e:
                print(f"{sym}: HTTP {e.code} {e.read().decode()[:160]}",
                      file=sys.stderr)
                return 1
            except Exception as e:
                print(f"{sym}: 网络异常 {e}(重试)", file=sys.stderr)
                time.sleep(2)
                continue
            pages += 1
            if not batch:
                break
            for rec in batch:
                rows[int(rec["fundingTime"])] = float(rec["fundingRate"])
            last_t = batch[-1]["fundingTime"]
            if len(batch) < PAGE:  # 到尾页
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
            print(f"{sym}: 新增 {len(fresh)} 条 / 全史 {len(seen)+len(fresh)} 条 "
                  f"({pages} 页,{yrs:.1f} 年,"
                  f"{dt.datetime.fromtimestamp(min(rows)/1000,dt.timezone.utc).strftime('%Y-%m-%d')} → "
                  f"{dt.datetime.fromtimestamp(max(rows)/1000,dt.timezone.utc).strftime('%Y-%m-%d')})")
        else:
            print(f"{sym}: 已最新({len(seen)} 条)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
