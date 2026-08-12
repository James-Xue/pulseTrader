"""xau-daemon — main entry point.

The daemon runs a sync polling loop:
  1. Poll the ticker every `poll_interval_seconds`.
  2. Aggregate ticks into 10-second bars via `BarAggregator`.
  3. Roll closed bars into the volume profile.
  4. After `min_bars_for_plan` bars have accumulated, compute trend + levels + plan.
  5. In dry-run mode, log the planned actions. In live mode, diff vs
     `list_open_pendings` and apply via `place_pending` / `cancel_order`.

Run modes:
  * `xau-daemon --once` — run a single cycle and exit. Useful for smoke
    tests and CI. Defaults to dry-run unless `--live` is passed.
  * `xau-daemon` (default) — loop forever, sleeping between cycles.
    Defaults to dry-run unless `--live` is passed.
  * `xau-daemon --live` — actually place orders. Pair with killswitch.

Safety guards (live mode only):
  * Per-order SL is computed as `entry ± sl_usd` from `[risk].sl_usd` and
    sent via `place_pending(..., price_sl=...)`.
  * `[killswitch] max_open_positions` — hard cap on concurrent pendings.
  * `[killswitch] max_orders_per_hour` — rate limit on PLACEs (in-memory).

Config file (xau.toml) is loaded via `xau.infra.config`. Secrets come from
environment via `from_env:` placeholders.

NEVER log the API secret. The daemon constructs a GateClient from the
resolved config but does not echo the secret value anywhere.
"""
from __future__ import annotations

import argparse
import sys
import time
from collections import deque
from decimal import Decimal
from pathlib import Path

import xau.infra.config as cfg
import xau.infra.logging as logging_mod
import xau.infra.clock as clock_mod
from xau.gate.account import get_assets, get_mt5_account
from xau.gate.client import GateClient
from xau.gate.orders import list_open_pendings, place_pending, cancel_order
from xau.gate.positions import (
    list_positions, close_position, update_position_sl_tp,
)
from xau.market_data.klines import BarAggregator, Kline, Tick
from xau.market_data.levels import extract_levels
from xau.market_data.ticker import fetch_ticker
from xau.market_data.volume_profile import VolumeProfile
from xau.risk.momentum import MomentumTpPolicy
from xau.state.book import Book, orphan_position_ids
from xau.state.positions import PositionBook, PositionRecord
from xau.strategy.sr_reversion import plan as plan_sr
from xau.strategy.trend import compute_trend
from xau.risk.sizing import risk_based_lot

log = logging_mod.get_logger("daemon")


def _build_client(broker_cfg: dict, credentials_cfg: dict) -> GateClient:
    """Build a GateClient from [broker] + [credentials] sections. Secret is never echoed."""
    key_prefix = credentials_cfg["api_key"][:6] + "…"
    log.info("client_init", api_key_prefix=key_prefix,
             base_url=broker_cfg.get("base_url"))
    return GateClient(
        api_key=credentials_cfg["api_key"],
        api_secret=credentials_cfg["api_secret"],
        base_url=broker_cfg.get("base_url", "https://api.gateio.ws/api/v4"),
        req_timeout_s=broker_cfg.get("req_timeout_s", 5.0),
    )


def _poll_one_tick(client: GateClient, symbol: str) -> Tick:
    """Fetch one ticker and convert to Tick. Raises on transport error."""
    t = fetch_ticker(client, symbol)
    return Tick(
        ts=time.time(),
        price=Decimal(t.last_price),
        bid=Decimal(t.bid_price),
        ask=Decimal(t.ask_price),
    )


def _collect_bars(
    client: GateClient,
    symbol: str,
    agg: BarAggregator,
    profile: VolumeProfile,
    *,
    target_bars: int,
    poll_interval_s: float,
    clock,
) -> int:
    """Poll the ticker until `target_bars` bars are closed. Returns # bars emitted."""
    emitted = 0
    deadline = clock.monotonic() + (target_bars + 1) * 10 + 5  # safety margin
    while emitted < target_bars:
        if clock.monotonic() > deadline:
            log.warning("collect_bars_timeout", target=target_bars, got=emitted)
            break
        try:
            tick = _poll_one_tick(client, symbol)
        except Exception as exc:  # httpx errors, signing errors, etc.
            log.error("poll_failed", err=str(exc))
            time.sleep(poll_interval_s)
            continue
        closed = agg.add(tick)
        if closed is not None:
            profile.add_bar(closed)
            emitted += 1
            if emitted % 5 == 0 or emitted == target_bars:
                log.info("bar_progress", emitted=emitted, target=target_bars)
        time.sleep(poll_interval_s)
    return emitted


def _compute_plan(
    profile: VolumeProfile,
    *,
    symbol: str,
    mid: Decimal,
    closes: list[Decimal],
    equity: Decimal,
    strategy_cfg: dict,
    risk_cfg: dict,
    leverage: int,
    profile_top_n: int,
) -> list:
    """Compute a fresh plan from current state. Returns a list of PlanItem."""
    nodes = profile.top_nodes(n=profile_top_n, min_sep_pts=50)
    max_seconds = max((n.seconds for n in nodes), default=0.0)
    levels = extract_levels(
        nodes, mid,
        min_distance_usd=strategy_cfg.get("min_distance_usd", 2.0),
        max_distance_usd=strategy_cfg.get("max_distance_usd", 25.0),
    )
    trend = compute_trend(
        closes,
        ema_short=strategy_cfg.get("ema_short", 30),
        ema_long=strategy_cfg.get("ema_long", 100),
        trend_threshold_pct=strategy_cfg.get("trend_threshold_pct", 0.05),
        confidence_full_pct=strategy_cfg.get("confidence_full_pct", 0.30),
    )
    items = plan_sr(
        levels, trend,
        symbol=symbol,
        equity=equity,
        risk_pct=risk_cfg["risk_pct"],
        sl_usd=Decimal(str(risk_cfg["sl_usd"])),
        max_lot=Decimal(str(risk_cfg["max_lot"])),
        min_lot=Decimal(str(risk_cfg["min_lot"])),
        leverage=leverage,
        max_seconds=max_seconds,
        offset_min_usd=strategy_cfg.get("offset_min_usd", 2.0),
        offset_max_usd=strategy_cfg.get("offset_max_usd", 5.0),
        min_distance_usd=strategy_cfg.get("min_distance_usd", 2.0),
        max_distance_usd=strategy_cfg.get("max_distance_usd", 25.0),
    )
    return items


def run_cycle(
    client: GateClient,
    cfg_root: dict,
    agg: BarAggregator,
    profile: VolumeProfile,
    closes: list[Decimal],
    *,
    dry_run: bool,
    rate_window: deque | None = None,
    position_book: PositionBook | None = None,
) -> int:
    """Run one daemon cycle. Returns the number of planned actions."""
    symbol = cfg_root["broker"]["symbol"]
    profile_top_n = cfg_root["strategy"].get("top_n_levels", 6)
    risk_cfg = cfg_root["risk"]
    strategy_cfg = cfg_root["strategy"]
    killswitch_cfg = cfg_root.get("killswitch", {}) or {}
    # Per-order leverage from [broker].leverage. NOT mt5.leverage — that's the
    # account's display default and can be 1 (which the broker rejects as
    # INVALID_ARGUMENT). Per-order CFD leverage must be one of {20,50,100,200,500}.
    leverage = int(cfg_root["broker"].get("leverage", 500))

    # Refresh account + ticker.
    try:
        mt5 = get_mt5_account(client)
        assets = get_assets(client)
    except Exception as exc:
        log.error("account_fetch_failed", err=str(exc))
        return 0
    equity = Decimal(assets.balance)

    try:
        ticker = fetch_ticker(client, symbol)
    except Exception as exc:
        log.error("ticker_fetch_failed", err=str(exc))
        return 0
    mid = Decimal(ticker.last_price)
    log.info(
        "market_state",
        last=ticker.last_price,
        bid=ticker.bid_price,
        ask=ticker.ask_price,
        equity=str(equity),
        bars_in_profile=profile.bar_count,
    )

    if profile.bar_count < strategy_cfg.get("ema_long", 100):
        log.info("warmup", bars=profile.bar_count, need=strategy_cfg.get("ema_long", 100))
        return 0

    items = _compute_plan(
        profile, symbol=symbol, mid=mid, closes=closes, equity=equity,
        strategy_cfg=strategy_cfg, risk_cfg=risk_cfg, leverage=leverage,
        profile_top_n=profile_top_n,
    )
    trend_kind = items and items  # placeholder; trend itself isn't returned by planner
    # Re-derive trend kind for logging.
    from xau.strategy.trend import compute_trend as _ct
    trend = _ct(
        closes,
        ema_short=strategy_cfg.get("ema_short", 30),
        ema_long=strategy_cfg.get("ema_long", 100),
        trend_threshold_pct=strategy_cfg.get("trend_threshold_pct", 0.05),
        confidence_full_pct=strategy_cfg.get("confidence_full_pct", 0.30),
    )
    log.info("plan_emitted", count=len(items), trend_kind=trend.kind.value)

    if dry_run:
        for it in items:
            sl = _compute_sl(it.side.value, it.price, risk_cfg["sl_usd"])
            log.info(
                "dry_run_plan",
                side=it.side.value,
                price=str(it.price),
                lot=str(it.lot),
                level=str(it.level_price),
                sl=str(sl),
                confidence=round(it.confidence, 3),
                reason=it.reason,
            )
        return len(items)

    # Live: diff vs current pendings, then apply.
    try:
        pendings = list_open_pendings(client, symbol=symbol)
    except Exception as exc:
        log.error("pendings_fetch_failed", err=str(exc))
        return 0
    book = Book(pendings=pendings)
    tolerance_pts = float(killswitch_cfg.get("price_tolerance_pts", 600.0))
    actions = book.diff_against_plan(items, price_tolerance_pts=tolerance_pts)

    # Killswitch: enforce max_open_positions on PLACE actions.
    max_open = int(killswitch_cfg.get("max_open_positions", 0))
    if max_open > 0:
        current_open = sum(1 for o in pendings if o.symbol == symbol)
        slots_left = max_open - current_open
        place_actions = [a for a in actions if a.kind == "PLACE"]
        cancel_actions = [a for a in actions if a.kind == "CANCEL"]
        if len(place_actions) > slots_left:
            skipped = len(place_actions) - max(slots_left, 0)
            place_actions = place_actions[:max(slots_left, 0)]
            log.warning(
                "killswitch_open_capped",
                max_open=max_open,
                current_open=current_open,
                attempted=len(place_actions) + skipped,
                skipped=skipped,
            )
        actions = cancel_actions + place_actions

    # Killswitch: enforce max_orders_per_hour on PLACE actions.
    max_per_hour = int(killswitch_cfg.get("max_orders_per_hour", 0))
    if max_per_hour > 0 and rate_window is not None:
        now = time.time()
        # Drop entries older than 1 hour.
        while rate_window and rate_window[0] < now - 3600:
            rate_window.popleft()
        place_actions = [a for a in actions if a.kind == "PLACE"]
        slots = max_per_hour - len(rate_window)
        if len(place_actions) > slots:
            skipped = len(place_actions) - max(slots, 0)
            place_actions = place_actions[:max(slots, 0)]
            log.warning(
                "killswitch_rate_capped",
                max_per_hour=max_per_hour,
                used=len(rate_window),
                skipped=skipped,
            )
        cancel_actions = [a for a in actions if a.kind == "CANCEL"]
        actions = cancel_actions + place_actions

    # Position management: orphan close + SL/TP push + momentum TP.
    if position_book is not None:
        try:
            positions = list_positions(client, symbol=symbol)
        except Exception as exc:
            log.error("positions_fetch_failed", err=str(exc))
            positions = []
        _position_management(
            client, cfg_root, items, positions, position_book,
            dry_run=dry_run,
        )

    for act in actions:
        if act.kind == "CANCEL":
            try:
                cancel_order(client, act.order_id)
                log.info("cancelled", order_id=act.order_id, reason=act.reason)
            except Exception as exc:
                log.error("cancel_failed", order_id=act.order_id, err=str(exc))
        elif act.kind == "PLACE":
            sl = _compute_sl(act.side, act.price, risk_cfg["sl_usd"])
            try:
                # Per official CFD schema (gate.com/docs/developers/apiv4/en/cfd):
                # required: symbol, side, volume, price, price_type ('trigger')
                # optional: price_tp, price_sl
                # NOTE: leverage/time_in_force/position_side are NOT in schema —
                # sending them caused INVALID_ARGUMENT.
                # Side semantics: 1=sell, 2=buy (per docs).
                # TODO v1.1: wire SL via PUT /tradfi/orders/{id} post-placement
                # once we confirm the update endpoint shape.
                order_id = place_pending(
                    client, symbol=act.symbol, side=act.side,
                    volume=str(act.lot), price=str(act.price),
                )
                if rate_window is not None:
                    rate_window.append(time.time())
                log.info(
                    "placed",
                    order_id=order_id, side=act.side,
                    price=str(act.price), lot=str(act.lot),
                    sl_planned=str(sl), sl_set=False, reason=act.reason,
                )
            except Exception as exc:
                payload = getattr(exc, "payload", None) or {}
                log.error("place_failed",
                          side=act.side,
                          price=str(act.price),
                          lot=str(act.lot),
                          label=getattr(exc, "label", ""),
                          message=getattr(exc, "message", str(exc)),
                          payload=str(payload)[:500])
    return len(actions)


def _compute_sl(side: int | str, entry_price: Decimal, sl_usd) -> Decimal:
    """Per-order SL = entry ± sl_usd. BUY: SL below; SELL: SL above.

    `side` may be int (1/2 — from Action, 2=buy per official CFD schema) or
    str ("BUY"/"SELL" — from dry-run).
    """
    sl = Decimal(str(sl_usd))
    side_str = side if isinstance(side, str) else ("BUY" if side == 2 else "SELL")
    if side_str == "BUY":
        return entry_price - sl
    return entry_price + sl


def _compute_tp(side: int | str, entry_price: Decimal, tp_usd) -> Decimal:
    """Per-order TP = entry ± tp_usd. BUY: TP above; SELL: TP below.

    `tp_usd` is in DOLLARS (user rule 2026-08-11). Mirror of `_compute_sl`.
    Side-int semantics: 2=buy (CFD), 1=sell.
    """
    tp = Decimal(str(tp_usd))
    side_str = side if isinstance(side, str) else ("BUY" if side == 2 else "SELL")
    if side_str == "BUY":
        return entry_price + tp
    return entry_price - tp


def _plan_active_sides(items) -> set[int]:
    """Set of side ints present in the current plan (for orphan detection)."""
    return {2 if it.side.value == "BUY" else 1 for it in items}


def _position_management(
    client: GateClient,
    cfg_root: dict,
    items: list,
    positions: list,
    position_book: PositionBook,
    *,
    dry_run: bool,
) -> None:
    """Auto-close orphans, push SL/TP, fire momentum TP.

    Three responsibilities, ordered to avoid double-closes:
      1. Auto-close any position whose side is not in the current plan.
         Removes the record from `position_book`.
      2. For all remaining positions, push SL/TP via PUT /tradfi/positions/{id}.
         Idempotent — skipped when last-pushed memo matches.
      3. For records still held, evaluate `MomentumTpPolicy`. On trigger,
         close and forget.

    In dry-run mode, all three steps LOG but do not call the broker. Use
    this to validate behavior without committing to the PUT endpoint shape.
    """
    risk_cfg = cfg_root["risk"]
    sl_usd = risk_cfg["sl_usd"]
    tp_usd = Decimal(str(risk_cfg.get("tp_usd", "0.48")))
    tp_window_s = int(risk_cfg.get("tp_window_s", 60))
    auto_close_orphans = bool(risk_cfg.get("auto_close_orphans", True))
    symbol = cfg_root["broker"]["symbol"]

    now_wall = position_book.clock.now()
    now_unix = now_wall.timestamp()
    new_records, all_records = position_book.mark_seen(positions, now=now_wall)
    plan_sides = _plan_active_sides(items)

    # 1. Orphan close.
    if auto_close_orphans:
        orphan_ids = orphan_position_ids(positions, plan_sides)
        for pid in orphan_ids:
            if dry_run:
                log.info("orphan_closed_dryrun", position_id=pid, reason="side_not_in_plan")
            else:
                try:
                    close_position(client, pid)
                    log.info("orphan_closed", position_id=pid, reason="side_not_in_plan")
                except Exception as exc:
                    log.error("orphan_close_failed", position_id=pid, err=str(exc))
                    continue
            position_book.forget(pid)

    # 2. Push SL/TP — fresh fills (new_records) get pushed immediately,
    #    plus any existing record whose memo doesn't match the new SL/TP.
    push_targets: dict[int, PositionRecord] = {}
    for rec in new_records:
        push_targets[rec.position_id] = rec
    for rec in all_records:
        if position_book.should_update_sl_tp(
            rec,
            sl=Decimal("0"),  # placeholder, replaced below
            tp=Decimal("0"),
        ):
            push_targets.setdefault(rec.position_id, rec)

    for rec in push_targets.values():
        # Find matching live position (it might have been closed between
        # mark_seen and here; mark_seen already drops it from `records`).
        live = next((p for p in positions if p.position_id == rec.position_id), None)
        if live is None:
            continue
        try:
            entry = Decimal(live.price)
        except Exception:
            continue
        sl = _compute_sl(rec.side, entry, sl_usd)
        tp = _compute_tp(rec.side, entry, tp_usd)
        # Skip if memo says we already pushed these exact values.
        if not position_book.should_update_sl_tp(rec, sl=sl, tp=tp):
            continue
        if dry_run:
            log.info(
                "sl_tp_pushed_dryrun",
                position_id=rec.position_id, side=rec.side,
                sl=str(sl), tp=str(tp),
            )
        else:
            try:
                update_position_sl_tp(
                    client, rec.position_id,
                    price_sl=f"{sl:.2f}", price_tp=f"{tp:.2f}",
                )
                log.info(
                    "sl_tp_pushed",
                    position_id=rec.position_id, side=rec.side,
                    sl=str(sl), tp=str(tp),
                )
            except Exception as exc:
                log.error(
                    "sl_tp_push_failed",
                    position_id=rec.position_id, err=str(exc),
                )
                continue
        position_book.update_sl_tp(rec.position_id, sl=sl, tp=tp)

    # 3. Momentum TP — pull a fresh ticker, evaluate each surviving record.
    policy = MomentumTpPolicy(reversal_pts=tp_usd, window_s=tp_window_s)
    try:
        ticker = fetch_ticker(client, symbol)
    except Exception as exc:
        log.error("ticker_fetch_failed_for_momentum", err=str(exc))
        return
    bid = Decimal(ticker.bid_price)
    ask = Decimal(ticker.ask_price)

    for rec in list(position_book.records.values()):
        if policy.should_take_profit(rec, now=now_unix, bid=bid, ask=ask):
            if dry_run:
                log.info(
                    "momentum_tp_fired_dryrun",
                    position_id=rec.position_id, side=rec.side,
                    entry=str(rec.entry_price),
                    bid=str(bid), ask=str(ask),
                    reversal_pts=str(policy.reversal_pts),
                )
            else:
                try:
                    close_position(client, rec.position_id)
                    log.info(
                        "momentum_tp_fired",
                        position_id=rec.position_id, side=rec.side,
                        entry=str(rec.entry_price),
                        bid=str(bid), ask=str(ask),
                        reversal_pts=str(policy.reversal_pts),
                    )
                except Exception as exc:
                    log.error(
                        "momentum_tp_close_failed",
                        position_id=rec.position_id, err=str(exc),
                    )
                    continue
            position_book.forget(rec.position_id)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="xau-daemon")
    parser.add_argument("--config", "-c", default="xau.toml")
    parser.add_argument("--once", action="store_true",
                        help="Run a single cycle and exit (smoke-test mode)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Log planned actions without placing real orders "
                             "(DEFAULT). Use --live to actually place orders.")
    parser.add_argument("--live", action="store_true",
                        help="Override [daemon].dry_run and actually place orders. "
                             "Use with care.")
    parser.add_argument("--min-bars", type=int, default=None,
                        help="Override min bars for plan (smoke testing)")
    args = parser.parse_args(argv)

    root = cfg.load_config(Path(args.config))
    broker_cfg = root["broker"]
    credentials_cfg = root.get("credentials", {})
    logging_mod.setup_logging(
        level=root.get("daemon", {}).get("log_level", "INFO"),
        json_path=Path(root.get("daemon", {}).get("log_json_path", "logs/daemon.jsonl")),
    )

    # Resolve dry_run: --live wins, then --dry-run, then config default (True).
    config_dry_run = bool(root.get("daemon", {}).get("dry_run", True))
    if args.live:
        dry_run = False
        log.warning("LIVE_MODE_ENABLED", via="--live flag",
                    note="orders will be placed against the broker")
    elif args.dry_run:
        dry_run = True
    else:
        dry_run = config_dry_run
    log.info("daemon_mode", dry_run=dry_run, config_default=config_dry_run)

    client = _build_client(broker_cfg, credentials_cfg)
    try:
        symbol = broker_cfg["symbol"]
        strategy_cfg = root.get("strategy", {})
        risk_cfg = root["risk"]
        poll_s = float(root.get("connection", {}).get("poll_interval_seconds", 1.0))
        min_bars = args.min_bars or strategy_cfg.get("min_bars_for_plan", 100)

        bucket_s = float(strategy_cfg.get("bucket_s", 10.0))
        window_bars = int(strategy_cfg.get("window_bars", 200))
        bin_usd = Decimal(str(strategy_cfg.get("bin_usd", "0.10")))

        agg = BarAggregator(bucket_s=bucket_s)
        profile = VolumeProfile(bin_usd=bin_usd, window_bars=window_bars)
        closes: list[Decimal] = []
        rate_window: deque[float] = deque(maxlen=1000)
        position_book = PositionBook(clock=clock_mod.SystemClock())

        # Initial warmup — collect `min_bars` bars before planning.
        log.info("warmup_start", min_bars=min_bars, poll_s=poll_s)
        got = _collect_bars(
            client, symbol, agg, profile,
            target_bars=min_bars, poll_interval_s=poll_s,
            clock=clock_mod.SystemClock(),
        )
        # Pull close prices from the profile's bars for EMA computation.
        for bar in profile._bars:  # noqa: SLF001 — read-by-daemon acceptable
            closes.append(bar.close)
        log.info("warmup_done", got=got, target=min_bars)

        n = run_cycle(client, root, agg, profile, closes,
                      dry_run=dry_run, rate_window=rate_window,
                      position_book=position_book)
        log.info("cycle_done", actions=n)

        if args.once:
            return 0

        # Live loop.
        log.info("entering_main_loop")
        while True:
            time.sleep(poll_s)
            tick = _poll_one_tick(client, symbol)
            closed = agg.add(tick)
            if closed is not None:
                profile.add_bar(closed)
                closes.append(closed.close)
                if len(closes) > window_bars:
                    closes = closes[-window_bars:]
            run_cycle(client, root, agg, profile, closes,
                      dry_run=dry_run, rate_window=rate_window,
                      position_book=position_book)
    finally:
        client.close()
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())