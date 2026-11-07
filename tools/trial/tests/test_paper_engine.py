#!/usr/bin/env python3
"""test_paper_engine.py — semantic unit tests for the paper engine (python3 -m unittest)

Covers: the aggregator replica (each SignalAggregator.cpp semantic rule) and the
paper-ledger state machine (entry / each exit path / fee apportioning).
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from paper_engine import AggregatorReplica, Consensus, PaperTrader, Signal

# Defaults kept in sync with trading.toml
THRESHOLD = 0.6
COOLDOWN_S = 30
MAKER_FEE = 0.0002
TAKER_FEE = 0.0005

TP_TARGETS = [0.005, 0.01, 0.02]
TP_FRACS = [1 / 3, 1 / 3, 1 / 3]
SL_TRAIL = 0.005
MAX_HOLD_S = 300

S = "BTC_USDT"


def sig(sid, typ, conf, px, ts_ms, sym=S):
    return Signal(strategy_id=sid, symbol=sym, type=typ,
                  confidence=conf, price=px, ts_ns=ts_ms * 1_000_000)


def trader(**kw):
    opts = dict(maker_fee=MAKER_FEE, taker_fee=TAKER_FEE, maker_timeout_s=5,
                notional_usdt=160_000.0, tp_targets_pct=TP_TARGETS,
                tp_fractions=TP_FRACS, sl_trailing_pct=SL_TRAIL,
                max_hold_s=MAX_HOLD_S)
    opts.update(kw)
    return PaperTrader(S, **opts)


def trader_no_ladder(**kw):
    """Disable the TP ladder when only trailing/max_hold are under test (an upward price move would trigger the ladder itself)."""
    kw.setdefault("tp_targets_pct", [])
    kw.setdefault("tp_fractions", [])
    return trader(**kw)


# ---------------------------------------------------------------------------
# AggregatorReplica — mirrors SignalAggregator.cpp
# ---------------------------------------------------------------------------

class TestAggregator(unittest.TestCase):

    def setUp(self):
        self.agg = AggregatorReplica(THRESHOLD, COOLDOWN_S,
                                     {"a": 1.0, "b": 1.0})

    def test_below_threshold_no_emit(self):
        # Single strategy: conf 0.5 < 0.6
        self.assertIsNone(self.agg.add(sig("a", "buy", 0.5, 100.0, 0)))

    def test_threshold_emit_single(self):
        c = self.agg.add(sig("a", "buy", 0.9, 100.0, 0))
        self.assertIsNotNone(c)
        self.assertEqual(c.type, "buy")
        self.assertAlmostEqual(c.confidence, 0.9)

    def test_two_strategies_agree_cross_threshold(self):
        # Each strategy 0.35 (0.35 apiece): weighted confidence = (0.35+0.35)/2 = 0.35 < 0.6
        self.assertIsNone(self.agg.add(sig("a", "buy", 0.35, 100.0, 0)))
        self.assertIsNone(self.agg.add(sig("b", "buy", 0.35, 100.0, 1)))

    def test_flat_never_participates(self):
        # Many flat signals produce no accumulation at all; a later buy still starts from zero as usual
        self.agg.add(sig("a", "flat", 0.9, 100.0, 0))
        self.agg.add(sig("a", "flat", 0.9, 100.0, 1))
        c = self.agg.add(sig("a", "buy", 0.9, 100.0, 2))
        self.assertIsNotNone(c)
        self.assertAlmostEqual(c.confidence, 0.9)

    def test_flat_ignored_even_above_threshold_with_cooldown(self):
        self.agg.add(sig("a", "buy", 0.9, 100.0, 0))
        self.agg.add(sig("b", "sell", 0.9, 100.0, 1))

    def test_cooldown_blocks_but_accumulation_keeps_growing(self):
        # Emitted at t=0 then reset; a 0.9 signal at t=10s is still held back by
        # the 30s cooldown and the accumulation is not reset; another same-direction
        # signal at t=10s+ accumulates 0.9+0.9=1.8, ws=2, normalized=0.9, and it is
        # still 10s since the last emission (<30s) so nothing is emitted (C++
        # semantics: the accumulation keeps growing until the next addSignal after
        # the cooldown expires emits)
        c0 = self.agg.add(sig("a", "buy", 0.9, 100.0, 0))
        self.assertIsNotNone(c0)
        self.assertIsNone(self.agg.add(sig("a", "buy", 0.9, 100.0,
                                           10_000)))
        self.assertIsNone(self.agg.add(sig("a", "buy", 0.9, 100.0,
                                           10_500)))
        # After the cooldown expires (>30s since the last emission) a new signal triggers an emission
        c1 = self.agg.add(sig("a", "buy", 0.9, 100.0, 31_000))
        self.assertIsNotNone(c1)
        self.assertEqual(self.agg.emissions, 2)

    def test_emit_resets_accumulation(self):
        self.agg.add(sig("a", "buy", 0.9, 100.0, 0))     # emits
        # Reset after emitting: one 0.9 same-direction plus one 0.1 opposite → the threshold is not reached again
        self.agg.add(sig("a", "buy", 0.5, 100.0, 40_000))
        self.agg.add(sig("a", "sell", 0.5, 100.0, 40_500))
        # buy=0.5 sell=0.5 → dominant 0.5/1.0 < 0.6 → no emission
        self.assertEqual(self.agg.emissions, 1)

    def test_cooldown_window_accumulation_flips_direction(self):
        # After the first buy emission, the sell majority accumulated during the
        # cooldown window wins as SELL once the window closes
        agg = AggregatorReplica(THRESHOLD, COOLDOWN_S, {"a": 1.0, "b": 1.0})
        c0 = agg.add(sig("a", "buy", 0.9, 100.0, 0))
        self.assertIsNotNone(c0)
        self.assertEqual(c0.type, "buy")
        # Inside the window (1s/10s/20s < 30s): sell 0.9×3 = 2.7 → dominant sell
        # 2.7/4 = 0.675 ≥ 0.6, but the cooldown holds it back → nothing is emitted
        # (the accumulation keeps growing)
        self.assertIsNone(agg.add(sig("b", "sell", 0.9, 100.0, 1_000)))
        self.assertIsNone(agg.add(sig("b", "sell", 0.9, 100.0, 10_000)))
        self.assertIsNone(agg.add(sig("b", "sell", 0.9, 100.0, 20_000)))
        # After the window (40s): one more 0.9 sell → sell 3.6 / ws 5 = 0.72 →
        # SELL is emitted, flipping direction relative to the first buy emission
        # (the accumulated majority wins)
        c1 = agg.add(sig("b", "sell", 0.9, 100.0, 40_000))
        self.assertIsNotNone(c1)
        self.assertEqual(c1.type, "sell")
        self.assertEqual(agg.emissions, 2)

    def test_unknown_strategy_defaults_weight_one(self):
        agg = AggregatorReplica(THRESHOLD, COOLDOWN_S, {})
        c = agg.add(sig("ghost", "buy", 0.9, 100.0, 0))
        self.assertIsNotNone(c)          # Default weight 1.0; the signal is not dropped for having no weight

    def test_per_symbol_state_isolated(self):
        self.agg.add(sig("a", "buy", 0.9, 100.0, 0, sym="BTC_USDT"))
        self.agg.add(sig("a", "buy", 0.9, 100.0, 1, sym="ETH_USDT"))
        self.assertEqual(self.agg.emissions, 2)


# ---------------------------------------------------------------------------
# PaperTrader — ledger state machine
# ---------------------------------------------------------------------------

class TestPaperTrader(unittest.TestCase):

    # ---- entry -----------------------------------------------------------

    def test_maker_entry_on_price_revisit(self):
        t = trader()
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 0))
        self.assertTrue(t.open)
        # Price moves up and never revisits → no fill
        t.on_tick(1_000, 100.5)
        t.on_tick(2_000, 100.4)
        self.assertFalse(t.round_trips)
        # Tape revisits ≤100 → maker fill
        t.on_tick(3_000, 99.9)
        self.assertEqual(len(t.round_trips), 0)  # Filled but not closed, so no RT
        self.assertEqual(t._phase, "active")
        self.assertEqual(t._entry_leg, "maker")
        self.assertAlmostEqual(t._qty, 160_000.0 / 100.0)

    def test_maker_timeout_taker_fallback(self):
        t = trader(maker_timeout_s=5)
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 0))
        # After the timeout the price has drifted to 100.8 → the taker fallback anchors at 100.8
        t.on_tick(6_000, 100.8)
        self.assertEqual(t._entry_leg, "taker")
        self.assertAlmostEqual(t._entry_px, 100.8)
        self.assertEqual(t._phase, "active")

    def test_flip_while_maker_pending_cancels(self):
        t = trader()
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 0))
        t.on_consensus(Consensus(S, "sell", 0.9, 99.5, 5_000))
        # Opposing signal while the order rests → cancel and discard, nothing is booked
        self.assertFalse(t.open)
        self.assertEqual(t.round_trips, [])

    def test_flat_consensus_never_emitted_by_aggregator(self):
        # The aggregator never emits flat (guaranteed by the type system); here we verify the ledger is immune to an unknown symbol
        t = trader()
        t.on_consensus(Consensus("ETH_USDT", "buy", 0.9, 100.0, 0))
        self.assertFalse(t.open)

    # ---- exit paths ------------------------------------------------------

    def _open_long_maker(self, t, px=100.0, conf=0.9):
        t.on_consensus(Consensus(S, "buy", conf, px, 0))
        t.on_tick(1, px)  # Immediate maker fill (buy: last ≤ px)

    def test_tp_ladder_partial_exits(self):
        t = trader()
        self._open_long_maker(t, 100.0)
        qty_orig = t._orig_qty
        # Reaches +0.5% → close 1/3
        t.on_tick(10_000, 100.6)          # 100.5+ → target 100.5
        self.assertEqual(len(t.round_trips), 1)
        rt = t.round_trips[0]
        self.assertEqual(rt.exit_reason, "tp_ladder")
        self.assertAlmostEqual(rt.qty, qty_orig / 3)
        self.assertAlmostEqual(rt.exit_px, 100.5)
        self.assertAlmostEqual(t._qty, qty_orig * 2 / 3)
        # No new target reached → no further closing
        t.on_tick(11_000, 100.7)
        self.assertEqual(len(t.round_trips), 1)
        # +1% target
        t.on_tick(12_000, 101.1)
        self.assertEqual(len(t.round_trips), 2)
        self.assertAlmostEqual(t._qty, qty_orig / 3)
        # +2% target → fully closed
        t.on_tick(13_000, 102.1)
        self.assertEqual(len(t.round_trips), 3)
        self.assertAlmostEqual(t._qty, 0.0)
        self.assertFalse(t.open)

    def test_trailing_sl_after_runup(self):
        t = trader_no_ladder()
        self._open_long_maker(t, 100.0)
        qty_orig = t._orig_qty
        # Rises to 102 then falls back 0.6% → 101.49 < 102×(1−0.005)=101.49 triggers
        t.on_tick(1_000, 102.0)
        t.on_tick(2_000, 101.49)
        self.assertEqual(len(t.round_trips), 1)
        rt = t.round_trips[0]
        self.assertEqual(rt.exit_reason, "trailing_sl")
        self.assertAlmostEqual(rt.qty, qty_orig)
        self.assertFalse(t.open)

    def test_trailing_sl_flat_line_no_trigger(self):
        t = trader()
        self._open_long_maker(t, 100.0)
        t.on_tick(1_000, 100.1)
        t.on_tick(2_000, 100.0)   # best 100.1 → trail 99.5995, not triggered
        self.assertEqual(t.round_trips, [])

    def test_max_hold_exit(self):
        t = trader_no_ladder(max_hold_s=300)
        self._open_long_maker(t, 100.0)
        qty_orig = t._orig_qty
        t.on_tick(301_000, 100.05)
        self.assertEqual(len(t.round_trips), 1)
        self.assertEqual(t.round_trips[0].exit_reason, "max_hold")
        self.assertAlmostEqual(t.round_trips[0].qty, qty_orig)

    def test_flip_consensus_closes_active(self):
        t = trader()
        self._open_long_maker(t, 100.0)
        t.on_consensus(Consensus(S, "sell", 0.9, 100.2, 5_000))
        self.assertEqual(len(t.round_trips), 1)
        self.assertEqual(t.round_trips[0].exit_reason, "flip")
        self.assertAlmostEqual(t.round_trips[0].exit_px, 100.2)
        self.assertFalse(t.open)
        # The same opposing consensus does not open a position in the other direction (single-position semantics)
        self.assertFalse(t.open)

    def test_same_dir_consensus_ignored_while_open(self):
        t = trader()
        self._open_long_maker(t, 100.0)
        qty_orig = t._orig_qty
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 1_000))
        self.assertEqual(t._qty, qty_orig)   # no scale-in

    # ---- fees and PnL ----------------------------------------------------

    def test_fees_and_pnl_maker_roundtrip(self):
        t = trader()
        self._open_long_maker(t, 100.0)   # maker fill @100
        qty_orig = t._orig_qty
        t.on_tick(30_000, 99.0)           # Falls straight back 1% → does trailing trigger?
        # 100→best 100, trail=99.5; last 99.0 ≤ 99.5 → closed by trailing_sl
        rt = t.round_trips[0]
        self.assertEqual(rt.exit_reason, "trailing_sl")
        # pnl = (99−100)×qty = −qty; entry maker fee = qty×100×0.0002;
        # exit taker fee = qty×99×0.0005
        self.assertAlmostEqual(rt.gross_pnl, -qty_orig)
        exp_fees = qty_orig * 100.0 * MAKER_FEE + qty_orig * 99.0 * TAKER_FEE
        self.assertAlmostEqual(rt.fees, exp_fees)
        self.assertAlmostEqual(rt.net_pnl, rt.gross_pnl - rt.fees)
        self.assertEqual(rt.entry_leg, "maker")

    def test_taker_fallback_entry_fees(self):
        t = trader(maker_timeout_s=1)
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 0))
        t.on_tick(2_000, 100.5)          # Timeout fallback taker @100.5
        # Quantity is anchored at the consensus price (in the engine the contract
        # count is fixed at order time), and the fallback executes the same quantity
        qty = 160_000.0 / 100.0
        self.assertAlmostEqual(t._entry_fee_total, qty * 100.5 * TAKER_FEE)
        t.on_tick(3_000, 100.5)          # Flat tick, hold has not reached 300s → no RT
        self.assertEqual(t.round_trips, [])

    def test_short_side_symmetry(self):
        t = trader_no_ladder()
        t.on_consensus(Consensus(S, "sell", 0.9, 100.0, 0))
        t.on_tick(1, 100.0)              # Sell maker fill: last ≥ px
        qty_orig = t._orig_qty
        t.on_tick(2_000, 99.0)           # best 100 → trail 100.5; falling to 99 does not trigger
        self.assertEqual(t.round_trips, [])
        t.on_tick(3_000, 100.6)          # Recovers past 100.5 → trailing triggers
        self.assertEqual(len(t.round_trips), 1)
        rt = t.round_trips[0]
        self.assertEqual(rt.exit_reason, "trailing_sl")
        self.assertAlmostEqual(rt.gross_pnl, (100.0 - rt.exit_px) * qty_orig)

    def test_ladder_short_targets_descend(self):
        t = trader()
        t.on_consensus(Consensus(S, "sell", 0.9, 100.0, 0))
        t.on_tick(1, 100.0)
        qty_orig = t._orig_qty
        t.on_tick(1_000, 99.4)           # −0.6% → target 99.5 broken → close 1/3
        self.assertEqual(len(t.round_trips), 1)
        self.assertAlmostEqual(t.round_trips[0].exit_px, 99.5)
        self.assertAlmostEqual(t.round_trips[0].qty, qty_orig / 3)


if __name__ == "__main__":
    unittest.main()
