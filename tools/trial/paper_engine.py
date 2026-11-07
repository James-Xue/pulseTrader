#!/usr/bin/env python3
"""paper_engine.py — core of the A/B weight paper-trading experiment (pure logic, no DB dependency)

Mirrors pulseTrader engine semantics so that any weighting strategy can be replayed
offline over a signal log:

  1. AggregatorReplica — a line-by-line replica of SignalAggregator.cpp:
     - Flat signals never contribute to accumulation
     - Per symbol, accumulate the weighted buy/sell confidence and weight_sum
     - normalized = dominant/weight_sum < threshold → do not emit
     - less than cooldown since the last emission → do not emit (C++ order:
       threshold first, then cooldown; while gated the accumulation is not reset
       and keeps growing)
     - after emitting, reset to zero and record last_signal; clock = signal-stream
       time (replay clock)
     - unregistered strategy weight = 1.0 (C++ getWeight default)

  2. PaperTrader — symbol-level single-position paper fill state machine:
     phase: None → maker (order resting until the price is touched) → active
     (position under monitoring) → None
     - maker-first entry: a limit order rests at P = consensus price; a tape
       revisit (buy: last ≤ P; sell: last ≥ P) fills it as maker; if it is not
       touched within maker_timeout_s → taker market fallback at the then-current
       last (engine M16 maker-first semantics)
     - an opposing consensus while the maker order rests → cancel and discard the
       order (nothing is booked without a fill)
     - during active, mirror StopLossEngine/TakeProfitEngine tick by tick:
       * TP ladder: last reaches targets_pct[i] → close fractions[i]×original
         quantity (taker)
       * Trailing SL: best is tracked; buy: last ≤ best×(1−trailing_pct) →
         close the remainder (taker)
       * max_hold_s exceeded → close the remainder (taker)
       * opposing consensus → close the remainder (taker); that signal does not
         open a new position in the opposite direction
     - a same-direction consensus is ignored while a position or order is open
       (single position, no scale-in)
     - fees: the maker leg pays maker_fee, every other leg pays taker_fee; the
       entry fee is apportioned across the RoundTrips by closed quantity (each
       ladder leg plus the final remainder close)

Timestamps are uniformly ns (consistent with signals.ts_ns); tick input is (ts_ms, last).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


# ---------------------------------------------------------------------------
# Signals and the aggregator replica
# ---------------------------------------------------------------------------

@dataclass
class Signal:
    strategy_id: str
    symbol: str
    type: str                 # "buy" / "sell" / "flat"
    confidence: float
    price: float
    ts_ns: int
    indicators: Optional[dict] = None
    reason: str = ""


@dataclass
class Consensus:
    """Consensus signal emitted by the aggregator (mirrors TradingSignal)."""
    symbol: str
    type: str                 # "buy" / "sell" (never flat)
    confidence: float
    price: float
    ts_ns: int


class AggregatorReplica:
    """Pure-function replica of SignalAggregator.cpp. State is kept per symbol;
    add returns the emitted Consensus (at most one) or None. The replay clock is
    the signal's own timestamp."""

    def __init__(self, threshold: float, cooldown_s: float,
                 weights: Dict[str, float]):
        self.threshold = threshold
        self.cooldown_ns = int(cooldown_s * 1_000_000_000)
        self.weights = dict(weights)
        # per-symbol accumulation state
        self._buy_sum: Dict[str, float] = {}
        self._sell_sum: Dict[str, float] = {}
        self._weight_sum: Dict[str, float] = {}
        self._last_price: Dict[str, float] = {}
        self._last_emit_ns: Dict[str, int] = {}
        self.emissions: int = 0

    def add(self, sig: Signal) -> Optional[Consensus]:
        if sig.type == "flat":
            return None
        weight = self.weights.get(sig.strategy_id, 1.0)
        sym = sig.symbol
        if sig.type == "buy":
            self._buy_sum[sym] = self._buy_sum.get(sym, 0.0) + sig.confidence * weight
        else:
            self._sell_sum[sym] = self._sell_sum.get(sym, 0.0) + sig.confidence * weight
        self._weight_sum[sym] = self._weight_sum.get(sym, 0.0) + weight
        self._last_price[sym] = sig.price
        return self._evaluate(sym, sig.ts_ns)

    def _evaluate(self, sym: str, now_ns: int) -> Optional[Consensus]:
        buy_sum = self._buy_sum.get(sym, 0.0)
        sell_sum = self._sell_sum.get(sym, 0.0)
        buy_dominant = buy_sum >= sell_sum
        dominant = buy_sum if buy_dominant else sell_sum
        ws = self._weight_sum.get(sym, 0.0)
        normalized = dominant if ws <= 0.0 else dominant / ws

        # 1. threshold gate (same order as the C++ code)
        if normalized < self.threshold:
            return None
        # 2. cooldown gate — while gated the accumulation is not reset and keeps
        #    growing (C++ semantics). A missing last_emit=None means it has never
        #    emitted (the replay clock may start at 0, so 0 cannot be used as the
        #    sentinel — the C++ code uses the wall clock nowMs(), so it has no
        #    such issue).
        last_emit = self._last_emit_ns.get(sym)
        if last_emit is not None \
                and now_ns - last_emit < self.cooldown_ns:
            return None

        # 3. Emit and reset
        self._buy_sum[sym] = 0.0
        self._sell_sum[sym] = 0.0
        self._weight_sum[sym] = 0.0
        self._last_emit_ns[sym] = now_ns
        self.emissions += 1
        return Consensus(
            symbol=sym,
            type="buy" if buy_dominant else "sell",
            confidence=max(0.0, min(1.0, normalized)),
            price=self._last_price.get(sym, 0.0),
            ts_ns=now_ns,
        )


# ---------------------------------------------------------------------------
# Paper ledger
# ---------------------------------------------------------------------------

@dataclass
class RoundTrip:
    entry_ts_ns: int
    exit_ts_ns: int
    direction: str              # "buy" (long) / "sell" (short)
    entry_px: float
    entry_leg: str              # "maker" / "taker"
    exit_px: float
    exit_reason: str            # "tp_ladder" / "trailing_sl" / "max_hold" / "flip"
    qty: float
    gross_pnl: float
    fees: float                 # apportioned entry fee + exit fee
    net_pnl: float
    notional: float
    conf_at_entry: float
    held_ms: int


class PaperTrader:
    """Symbol-level paper fill state machine. Event interface: on_consensus / on_tick."""

    def __init__(self, symbol: str,
                 maker_fee: float, taker_fee: float,
                 maker_timeout_s: float,
                 notional_usdt: float,
                 tp_targets_pct: List[float],
                 tp_fractions: List[float],
                 sl_trailing_pct: float,
                 max_hold_s: float):
        self.symbol = symbol
        self.maker_fee = maker_fee
        self.taker_fee = taker_fee
        self.maker_timeout_ns = int(maker_timeout_s * 1_000_000_000)
        self.notional = notional_usdt
        self.tp_targets = list(tp_targets_pct)
        self.tp_fractions = list(tp_fractions)
        self.sl_trailing_pct = sl_trailing_pct
        self.max_hold_ns = int(max_hold_s * 1_000_000_000)

        self.round_trips: List[RoundTrip] = []

        # position state (phase: None | "maker" | "active")
        self._phase: Optional[str] = None
        self._dir: Optional[str] = None
        self._qty: float = 0.0           # remaining quantity
        self._orig_qty: float = 0.0
        self._entry_ts: int = 0
        self._entry_px: float = 0.0
        self._entry_fee_total: float = 0.0
        self._entry_leg: str = "taker"
        self._best_px: float = 0.0
        self._next_tp: int = 0
        self._conf: float = 0.0
        self._maker_deadline_ns: int = 0

    # ---- events -----------------------------------------------------------

    def on_consensus(self, c: Consensus):
        if c.symbol != self.symbol:
            return
        if self._phase is None:
            # fresh entry: maker-first limit at consensus price
            self._phase = "maker"
            self._dir = c.type
            self._entry_px = c.price if c.price > 0 else 0.0
            self._qty = self.notional / self._entry_px if self._entry_px > 0 else 0.0
            self._orig_qty = self._qty
            self._entry_ts = c.ts_ns
            self._entry_fee_total = 0.0
            self._best_px = self._entry_px
            self._next_tp = 0
            self._conf = c.confidence
            self._maker_deadline_ns = c.ts_ns + self.maker_timeout_ns
        elif self._phase == "maker":
            # Opposing consensus while the order rests → cancel and discard it
            # (nothing is booked without a fill)
            if c.type != self._dir:
                self._clear_position()
        else:  # active
            if c.type != self._dir:
                self._book_close(c.price, "flip", c.ts_ns)

    def on_tick(self, ts_ms: int, last: float):
        """Evaluate tick by tick: maker touch, taker fallback, TP ladder, trailing, max_hold."""
        ts_ns = ts_ms * 1_000_000
        if self._phase == "maker":
            if self._dir == "buy" and last <= self._entry_px:
                self._establish(ts_ns, self._entry_px, "maker")
            elif self._dir == "sell" and last >= self._entry_px:
                self._establish(ts_ns, self._entry_px, "maker")
            elif ts_ns >= self._maker_deadline_ns:
                # Timeout → taker market fallback (engine M16: market fill after the timeout)
                self._establish(ts_ns, last, "taker")
        elif self._phase == "active":
            self._evaluate_exits(ts_ns, last)

    # ---- internal ---------------------------------------------------------

    def _establish(self, ts_ns: int, fill_px: float, leg: str):
        """Order filled or fallback executed → active. The entry price anchors to the actual fill price."""
        self._entry_px = fill_px
        self._best_px = fill_px
        self._entry_leg = leg
        self._entry_fee_total = self._qty * fill_px * (
            self.maker_fee if leg == "maker" else self.taker_fee)
        self._phase = "active"
        self._maker_deadline_ns = 0

    def _clear_position(self):
        self._phase = None
        self._dir = None
        self._qty = 0.0
        self._orig_qty = 0.0

    def _evaluate_exits(self, ts_ns: int, last: float):
        if self._dir == "buy":
            if last > self._best_px:
                self._best_px = last
            # TP ladder (each level triggers as its target price is reached, in ascending order)
            while (self._next_tp < len(self.tp_targets)
                   and self._qty > 0):
                target_px = self._entry_px * (1.0 + self.tp_targets[self._next_tp])
                if last >= target_px:
                    qty = self._orig_qty * self.tp_fractions[self._next_tp]
                    qty = min(qty, self._qty)
                    self._book_leg(qty, target_px, "tp_ladder", ts_ns)
                    self._next_tp += 1
                else:
                    break
            # trailing stop
            if self._qty > 0 and self.sl_trailing_pct > 0:
                if last <= self._best_px * (1.0 - self.sl_trailing_pct):
                    self._book_close(last, "trailing_sl", ts_ns)
                    return
            # max hold
            if self._qty > 0 and ts_ns - self._entry_ts >= self.max_hold_ns:
                self._book_close(last, "max_hold", ts_ns)
        else:  # short
            if last < self._best_px:
                self._best_px = last
            while (self._next_tp < len(self.tp_targets)
                   and self._qty > 0):
                target_px = self._entry_px * (1.0 - self.tp_targets[self._next_tp])
                if last <= target_px:
                    qty = self._orig_qty * self.tp_fractions[self._next_tp]
                    qty = min(qty, self._qty)
                    self._book_leg(qty, target_px, "tp_ladder", ts_ns)
                    self._next_tp += 1
                else:
                    break
            if self._qty > 0 and self.sl_trailing_pct > 0:
                if last >= self._best_px * (1.0 + self.sl_trailing_pct):
                    self._book_close(last, "trailing_sl", ts_ns)
                    return
            if self._qty > 0 and ts_ns - self._entry_ts >= self.max_hold_ns:
                self._book_close(last, "max_hold", ts_ns)

    def _book_leg(self, qty: float, px: float, reason: str, ts_ns: int):
        """Book one closing leg (shared by the ladder and the final close) and apportion the entry fee by qty."""
        if qty <= 0:
            return
        exit_fee = qty * px * self.taker_fee
        entry_fee_share = self._entry_fee_total * (qty / self._orig_qty) \
            if self._orig_qty > 0 else 0.0
        pnl = ((px - self._entry_px) * qty) if self._dir == "buy" \
            else ((self._entry_px - px) * qty)
        self.round_trips.append(RoundTrip(
            entry_ts_ns=self._entry_ts, exit_ts_ns=ts_ns,
            direction=self._dir, entry_px=self._entry_px,
            entry_leg=self._entry_leg, exit_px=px, exit_reason=reason,
            qty=qty, gross_pnl=pnl, fees=entry_fee_share + exit_fee,
            net_pnl=pnl - entry_fee_share - exit_fee,
            notional=qty * px, conf_at_entry=self._conf,
            held_ms=(ts_ns - self._entry_ts) // 1_000_000,
        ))
        self._qty -= qty
        # When the ladder fractions sum to 1.0 the final leg flattens the position;
        # the 1/3×3 floating-point residue is ~1e-9 in relative terms, so compare
        # against a relative epsilon.
        if self._qty <= 1e-9 * self._orig_qty:
            self._qty = 0.0
            self._clear_position()

    def _book_close(self, px: float, reason: str, ts_ns: int):
        if self._qty <= 0:
            return
        self._book_leg(self._qty, px, reason, ts_ns)
        self._clear_position()

    @property
    def open(self) -> bool:
        return self._phase is not None
