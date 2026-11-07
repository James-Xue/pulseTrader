#!/usr/bin/env python3
"""run_replay.py — A/B weight paper-replay CLI

Replays the engine's aggregation semantics (AggregatorReplica) plus paper fills
(PaperTrader) over the signal log (signals table) for a given symbol under a
given weighting scheme, and emits the ledger and a daily report.

Usage:
  python3 tools/trial/run_replay.py --arm equal [--symbol BTC_USDT]
  python3 tools/trial/run_replay.py --arm ai --weights-file weights.json
  python3 tools/trial/run_replay.py --arm equal --since-ns 1752000000000000000

Defaults match trading.toml (threshold 0.6 / cooldown 30s / trailing SL 0.5% /
TP ladder 0.5-1-2% ×1/3 / max_hold 300s / maker 0.02% / taker 0.05%); passing
--config trading.toml reads them from that file (until that is implemented, the
built-in defaults plus the CLI flags are used).

Outputs (data/trial/<symbol>/<arm>/):
  round_trips.csv   — per round trip: entry/exit/direction/price/leg/reason/PnL/fees
  daily_summary.json — per day: round trips/gross/net/win rate/fee share
  open_at_cutoff.json — still holding at the end (excluded from the stats, listed separately)
Prints the daily report table and a summary line to stdout.
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

# Defaults match trading.toml.
DEF_THRESHOLD = 0.6
DEF_COOLDOWN_S = 30
DEF_MAKER_FEE = 0.0002
DEF_TAKER_FEE = 0.0005
DEF_TP_TARGETS = [0.005, 0.01, 0.02]
DEF_TP_FRACS = [1 / 3, 1 / 3, 1 / 3]
DEF_SL_TRAIL = 0.005
DEF_MAX_HOLD_S = 300
DEF_MAKER_TIMEOUT_S = 5.0      # M16 maker-first timeout, market-order fallback
DEF_NOTIONAL = 160_000.0       # ≈ 2 BTC perpetual contracts at 80k


def load_config(path: Optional[str]) -> dict:
    """Read the [strategy] / [risk] defaults from trading.toml (fall back to the code defaults when absent)."""
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
    assert isinstance(w, dict) and w, \
        "weights must be a non-empty {strategy_id: float} object"
    total = sum(w.values())
    assert total > 0
    # Normalise (design constraint: the weights sum to 1 within a group;
    # normalising in the replay does not change the direction).
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
        # Feed the ticks preceding the signal instant to the ledger first
        # (exiting an open position needs the price path).
        while next_tick is not None and next_tick[0] * 1_000_000 <= sig.ts_ns:
            trader.on_tick(next_tick[0], next_tick[1])
            next_tick = next(ticks, None)
        c = agg.add(sig)
        if c is not None:
            trader.on_consensus(c)

    # Remaining tail ticks (once the signal stream ends, walk the rest of the
    # price path so open positions can exit normally up to the cutoff).
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

    # CSV ledger
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

    # Per-day summary
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

    # stdout report
    print(f"\n=== {arm} @ {symbol} (threshold={cfg['threshold']}, "
          f"cooldown={cfg['cooldown_s']}s) ===")
    if not rts:
        print("  no round trips (insufficient signals, or the weighting "
              "scheme never produced consensus)")
        return
    tot_gross = sum(r.gross_pnl for r in rts)
    tot_fees = sum(r.fees for r in rts)
    tot_net = sum(r.net_pnl for r in rts)
    wins = sum(1 for r in rts if r.net_pnl > 0)
    print(f"  round trips {len(rts)} | gross {tot_gross:+.2f} | "
          f"fees {tot_fees:.2f} | net {tot_net:+.2f} USDT | "
          f"win rate {wins/len(rts):.1%}")
    print(f"  expectancy per round trip (net): {tot_net/len(rts):+.4f} USDT | "
          f"fee share: {tot_fees/(abs(tot_gross)+1e-9):.1%}")
    exit_reasons: dict = defaultdict(int)
    for r in rts:
        exit_reasons[r.exit_reason] += 1
    print(f"  exit path distribution: {dict(exit_reasons)}")
    print(f"  daily report → {out_dir}/daily_summary.json | "
          f"ledger → {out_dir}/round_trips.csv")


def main() -> int:
    p = argparse.ArgumentParser(description="A/B weight paper replay")
    p.add_argument("--db", default="data/trades.db")
    p.add_argument("--symbol", default="BTC_USDT")
    p.add_argument("--arm", default="equal", choices=["equal", "ai"])
    p.add_argument("--weights-file",
                   help="AI-arm weights JSON {strategy_id: w} "
                        "(normalised automatically)")
    p.add_argument("--config", default="trading.toml",
                   help="read the [strategy]/[risk] defaults; fall back to "
                        "the built-in defaults if the file is absent")
    p.add_argument("--since-ns", type=int, default=0)
    p.add_argument("--until-ns", type=int, default=0, help="0 = up to now")
    p.add_argument("--notional", type=float, default=DEF_NOTIONAL)
    p.add_argument("--maker-fee", type=float, default=DEF_MAKER_FEE)
    p.add_argument("--taker-fee", type=float, default=DEF_TAKER_FEE)
    args = p.parse_args()

    if args.arm == "ai" and not args.weights_file:
        print("--arm ai requires --weights-file", file=sys.stderr)
        return 2

    cfg = load_config(args.config)
    cfg["maker_timeout_s"] = DEF_MAKER_TIMEOUT_S  # No TOML key; fixed default

    if not os.path.exists(args.db):
        print(f"DB does not exist: {args.db} (signal journaling has not "
              f"started — deploy M32 first)",
              file=sys.stderr)
        return 2

    weights = (load_weights(args.weights_file) if args.arm == "ai"
               else {})  # equal: nothing given → the aggregator default of 1.0
    agg, trader, n_signals = run_arm(
        args.db, args.symbol, weights, cfg,
        since_ns=args.since_ns, until_ns=args.until_ns,
        notional_usdt=args.notional, maker_fee=args.maker_fee,
        taker_fee=args.taker_fee)
    print(f"  signal stream: {n_signals} signals → "
          f"{agg.emissions} aggregator emissions")
    summarize(trader, agg, args.symbol, args.arm, cfg, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
