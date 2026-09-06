#!/usr/bin/env python3
"""run_replay.py — A/B 权重纸面重放 CLI

在信号日志(signals 表)上对指定 symbol 以指定权重策略重放引擎
聚合语义(AgregatorReplica)+ 纸面成交(PaperTrader),输出账本与日报。

用法:
  python3 tools/trial/run_replay.py --arm equal [--symbol BTC_USDT]
  python3 tools/trial/run_replay.py --arm ai --weights-file weights.json
  python3 tools/trial/run_replay.py --arm equal --since-ns 1752000000000000000

默认参数与 trading.toml 一致(threshold 0.6 / cooldown 30s / trailing
SL 0.5% / TP 阶梯 0.5-1-2% ×1/3 / max_hold 300s / maker 0.02% / taker
0.05%);--config 指定 trading.toml 则从中读取(尚未实现时用默认+CLI)。

输出(data/trial/<symbol>/<arm>/):
  round_trips.csv   — 每回合:入场/出场/方向/价格/腿/原因/PnL/费用
  daily_summary.json — 逐日:回合数/毛/净/胜率/费用占比
  open_at_cutoff.json — 结束时仍持仓(不计入统计,单独列出)
stdout 打印日报表 + 汇总结论行。
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sqlite3
import sys
from collections import defaultdict
from datetime import datetime, timezone
from typing import Dict, Iterator, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from paper_engine import AggregatorReplica, Consensus, PaperTrader, Signal

# 默认与 trading.toml 一致
DEF_THRESHOLD = 0.6
DEF_COOLDOWN_S = 30
DEF_MAKER_FEE = 0.0002
DEF_TAKER_FEE = 0.0005
DEF_TP_TARGETS = [0.005, 0.01, 0.02]
DEF_TP_FRACS = [1 / 3, 1 / 3, 1 / 3]
DEF_SL_TRAIL = 0.005
DEF_MAX_HOLD_S = 300
DEF_MAKER_TIMEOUT_S = 5.0      # M16 maker-first 超时市价兜底
DEF_NOTIONAL = 160_000.0       # ≈ 2 张 BTC 永续 @80k


def load_config(path: Optional[str]) -> dict:
    """从 trading.toml 读 [strategy] / [risk] 相关默认(缺失时用代码默认)。"""
    out = {}
    if not path or not os.path.exists(path):
        return out
    try:
        import tomllib
    except ImportError:  # py<3.11
        return out
    with open(path, "rb") as f:
        cfg = tomllib.load(f)
    st = cfg.get("strategy", {})
    out["threshold"] = st.get("signal_aggregator_threshold", DEF_THRESHOLD)
    out["cooldown_s"] = st.get("signal_cooldown_sec", DEF_COOLDOWN_S)
    sl = cfg.get("risk", {}).get("stop_loss", {})
    out["sl_trailing_pct"] = sl.get("trailing_pct", DEF_SL_TRAIL)
    out["max_hold_s"] = sl.get("max_hold_seconds", DEF_MAX_HOLD_S)
    tp = cfg.get("risk", {}).get("take_profit", {})
    out["tp_targets"] = tp.get("targets_pct", DEF_TP_TARGETS)
    out["tp_fractions"] = tp.get("fractions", DEF_TP_FRACS)
    return out


def load_weights(path: str) -> Dict[str, float]:
    with open(path, encoding="utf-8") as f:
        w = json.load(f)
    assert isinstance(w, dict) and w, "weights 必须是 {strategy_id: float} 非空对象"
    total = sum(w.values())
    assert total > 0
    # 归一化(设计约束:组内求和 = 1,replay 归一化不影响方向)
    return {k: v / total for k, v in w.items()}


def iter_signals(db_path: str, symbol: str,
                 since_ns: int, until_ns: int) -> Iterator[Signal]:
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    sql = ("SELECT ts_ns, strategy_id, symbol, market_type, type, confidence, "
           "price, reason, indicators FROM signals "
           "WHERE symbol = ? AND strategy_id != 'signal_aggregator' "
           "AND ts_ns BETWEEN ? AND ? ORDER BY id")
    try:
        for row in con.execute(sql, (symbol, since_ns, until_ns)):
            yield Signal(
                strategy_id=row[1], symbol=row[2], type=row[4],
                confidence=row[5], price=row[6], ts_ns=row[0],
                reason=row[7],
                indicators=json.loads(row[8]) if row[8] else None)
    finally:
        con.close()


def tick_reader(db_path: str, symbol: str,
                since_ms: int, until_ms: int) -> Iterator[Tuple[int, float]]:
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    sql = ("SELECT ts_ms, last FROM ticker_ticks "
           "WHERE symbol = ? AND ts_ms BETWEEN ? AND ? ORDER BY ts_ms")
    try:
        for ts_ms, last in con.execute(sql, (symbol, since_ms, until_ms)):
            yield int(ts_ms), float(last)
    finally:
        con.close()


def run_arm(db_path: str, symbol: str, weights: Dict[str, float],
            cfg: dict, since_ns: int = 0, until_ns: int = 0,
            notional_usdt: float = DEF_NOTIONAL,
            maker_fee: float = DEF_MAKER_FEE,
            taker_fee: float = DEF_TAKER_FEE) -> tuple:
    since_ms = since_ns // 1_000_000
    until_ms = until_ns // 1_000_000 if until_ns else (1 << 62)

    agg = AggregatorReplica(cfg["threshold"], cfg["cooldown_s"], weights)
    trader = PaperTrader(
        symbol=symbol,
        maker_fee=maker_fee, taker_fee=taker_fee,
        maker_timeout_s=cfg.get("maker_timeout_s", DEF_MAKER_TIMEOUT_S),
        notional_usdt=notional_usdt,
        tp_targets_pct=cfg["tp_targets"], tp_fractions=cfg["tp_fractions"],
        sl_trailing_pct=cfg["sl_trailing_pct"], max_hold_s=cfg["max_hold_s"])

    signals = iter_signals(db_path, symbol, since_ns, until_ns or (1 << 62))
    ticks = tick_reader(db_path, symbol, since_ms, until_ms)
    next_tick = next(ticks, None)
    n_signals = 0

    for sig in signals:
        n_signals += 1
        # 先把信号时刻之前的 tick 喂给账本(持仓退出需要价格路径)
        while next_tick is not None and next_tick[0] * 1_000_000 <= sig.ts_ns:
            trader.on_tick(next_tick[0], next_tick[1])
            next_tick = next(ticks, None)
        c = agg.add(sig)
        if c is not None:
            trader.on_consensus(c)

    # 尾部剩余 tick(信号流结束后价格路径走完,让 open 仓正常退出到 cutoff)
    while next_tick is not None:
        trader.on_tick(next_tick[0], next_tick[1])
        next_tick = next(ticks, None)

    return agg, trader, n_signals


def day_of(ns: int) -> str:
    return datetime.fromtimestamp(ns / 1_000_000_000, tz=timezone.utc) \
        .strftime("%Y-%m-%d")


def summarize(trader: PaperTrader, agg: AggregatorReplica, symbol: str,
              arm: str, cfg: dict, args) -> None:
    rts = trader.round_trips
    out_dir = os.path.join("data", "trial", symbol, arm)
    os.makedirs(out_dir, exist_ok=True)

    # CSV 账本
    with open(os.path.join(out_dir, "round_trips.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["entry_ts_ns", "exit_ts_ns", "direction", "entry_px",
                    "entry_leg", "exit_px", "exit_reason", "qty", "gross_pnl",
                    "fees", "net_pnl", "notional", "conf_at_entry", "held_ms"])
        for rt in rts:
            w.writerow([rt.entry_ts_ns, rt.exit_ts_ns, rt.direction,
                        rt.entry_px, rt.entry_leg, rt.exit_px, rt.exit_reason,
                        rt.qty, rt.gross_pnl, rt.fees, rt.net_pnl,
                        rt.notional, rt.conf_at_entry, rt.held_ms])

    # 逐日汇总
    by_day: dict = defaultdict(lambda: {"n": 0, "gross": 0.0, "fees": 0.0,
                                        "net": 0.0, "wins": 0})
    for rt in rts:
        d = by_day[day_of(rt.exit_ts_ns)]
        d["n"] += 1
        d["gross"] += rt.gross_pnl
        d["fees"] += rt.fees
        d["net"] += rt.net_pnl
        d["wins"] += 1 if rt.net_pnl > 0 else 0

    rows = []
    for d in sorted(by_day):
        v = by_day[d]
        rows.append({
            "day": d, "round_trips": v["n"], "gross_pnl": round(v["gross"], 4),
            "fees": round(v["fees"], 4), "net_pnl": round(v["net"], 4),
            "win_rate": round(v["wins"] / v["n"], 4) if v["n"] else None,
        })
    with open(os.path.join(out_dir, "daily_summary.json"), "w") as f:
        json.dump(rows, f, indent=2)

    # stdout 报告
    print(f"\n=== {arm} @ {symbol} (threshold={cfg['threshold']}, "
          f"cooldown={cfg['cooldown_s']}s) ===")
    if not rts:
        print("  无回合(信号不足或权重策略未产生共识)")
        return
    tot_gross = sum(r.gross_pnl for r in rts)
    tot_fees = sum(r.fees for r in rts)
    tot_net = sum(r.net_pnl for r in rts)
    wins = sum(1 for r in rts if r.net_pnl > 0)
    print(f"  回合 {len(rts)} | 毛 {tot_gross:+.2f} | 费 {tot_fees:.2f} | "
          f"净 {tot_net:+.2f} USDT | 胜率 {wins/len(rts):.1%}")
    print(f"  每回合期望(净): {tot_net/len(rts):+.4f} USDT | "
          f"费用占比: {tot_fees/(abs(tot_gross)+1e-9):.1%}")
    exit_reasons: dict = defaultdict(int)
    for r in rts:
        exit_reasons[r.exit_reason] += 1
    print(f"  退出路径分布: {dict(exit_reasons)}")
    print(f"  日报 → {out_dir}/daily_summary.json | 账本 → {out_dir}/round_trips.csv")


def main() -> int:
    p = argparse.ArgumentParser(description="A/B 权重纸面重放")
    p.add_argument("--db", default="data/trades.db")
    p.add_argument("--symbol", default="BTC_USDT")
    p.add_argument("--arm", default="equal", choices=["equal", "ai"])
    p.add_argument("--weights-file",
                   help="AI 臂权重 JSON {strategy_id: w}(自动归一化)")
    p.add_argument("--config", default="trading.toml",
                   help="读取 [strategy]/[risk] 默认;不存在则用内置默认")
    p.add_argument("--since-ns", type=int, default=0)
    p.add_argument("--until-ns", type=int, default=0, help="0 = 至今")
    p.add_argument("--notional", type=float, default=DEF_NOTIONAL)
    p.add_argument("--maker-fee", type=float, default=DEF_MAKER_FEE)
    p.add_argument("--taker-fee", type=float, default=DEF_TAKER_FEE)
    args = p.parse_args()

    if args.arm == "ai" and not args.weights_file:
        print("--arm ai 需要 --weights-file", file=sys.stderr)
        return 2

    cfg = load_config(args.config)
    cfg["maker_timeout_s"] = DEF_MAKER_TIMEOUT_S  # 无 toml 项,固定默认

    if not os.path.exists(args.db):
        print(f"DB 不存在: {args.db} (信号日志尚未开始,先部署 M32)",
              file=sys.stderr)
        return 2

    weights = (load_weights(args.weights_file) if args.arm == "ai"
               else {})  # equal: 全缺省 → 聚合器默认权重 1.0
    agg, trader, n_signals = run_arm(
        args.db, args.symbol, weights, cfg,
        since_ns=args.since_ns, until_ns=args.until_ns,
        notional_usdt=args.notional, maker_fee=args.maker_fee,
        taker_fee=args.taker_fee)
    print(f"  信号流: {n_signals} 条 → 聚合发射 {agg.emissions} 次")
    summarize(trader, agg, args.symbol, args.arm, cfg, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
