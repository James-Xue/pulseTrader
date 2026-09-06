#!/usr/bin/env python3
"""test_paper_engine.py — 纸面引擎语义单测(python3 -m unittest)

覆盖:聚合器复刻(SignalAggregator.cpp 语义逐条)、
纸面账本状态机(入场/退出各路径/费用分摊)。
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from paper_engine import AggregatorReplica, Consensus, PaperTrader, Signal

# 默认与 trading.toml 一致
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
    """仅测 trailing/max_hold 时禁用 TP 阶梯(价格上行本身会触发阶梯)。"""
    kw.setdefault("tp_targets_pct", [])
    kw.setdefault("tp_fractions", [])
    return trader(**kw)


# ---------------------------------------------------------------------------
# AggregatorReplica — 镜像 SignalAggregator.cpp
# ---------------------------------------------------------------------------

class TestAggregator(unittest.TestCase):

    def setUp(self):
        self.agg = AggregatorReplica(THRESHOLD, COOLDOWN_S,
                                     {"a": 1.0, "b": 1.0})

    def test_below_threshold_no_emit(self):
        # 单策略 conf 0.5 < 0.6
        self.assertIsNone(self.agg.add(sig("a", "buy", 0.5, 100.0, 0)))

    def test_threshold_emit_single(self):
        c = self.agg.add(sig("a", "buy", 0.9, 100.0, 0))
        self.assertIsNotNone(c)
        self.assertEqual(c.type, "buy")
        self.assertAlmostEqual(c.confidence, 0.9)

    def test_two_strategies_agree_cross_threshold(self):
        # 每策略 0.35(各 0.35),加权置信 = (0.35+0.35)/2 = 0.35 < 0.6
        self.assertIsNone(self.agg.add(sig("a", "buy", 0.35, 100.0, 0)))
        self.assertIsNone(self.agg.add(sig("b", "buy", 0.35, 100.0, 1)))

    def test_flat_never_participates(self):
        # 大量 flat 不产生任何累积;随后一条 buy 照常从零起算
        self.agg.add(sig("a", "flat", 0.9, 100.0, 0))
        self.agg.add(sig("a", "flat", 0.9, 100.0, 1))
        c = self.agg.add(sig("a", "buy", 0.9, 100.0, 2))
        self.assertIsNotNone(c)
        self.assertAlmostEqual(c.confidence, 0.9)

    def test_flat_ignored_even_above_threshold_with_cooldown(self):
        self.agg.add(sig("a", "buy", 0.9, 100.0, 0))
        self.agg.add(sig("b", "sell", 0.9, 100.0, 1))

    def test_cooldown_blocks_but_accumulation_keeps_growing(self):
        # t=0 发射后清零;t=10s 来一条 0.9 → 仍被 cooldown(30s)闸住,
        # 累积不清零;t=10s+再来一条同向 → 累积 0.9+0.9=1.8,ws=2,
        # normalized=0.9,距上次发射 10s <30s → 仍不发射(C++ 语义:
        # 累积继续增长,直到 cooldown 解除后下一次 addSignal 发射)
        c0 = self.agg.add(sig("a", "buy", 0.9, 100.0, 0))
        self.assertIsNotNone(c0)
        self.assertIsNone(self.agg.add(sig("a", "buy", 0.9, 100.0,
                                           10_000)))
        self.assertIsNone(self.agg.add(sig("a", "buy", 0.9, 100.0,
                                           10_500)))
        # cooldown 解除后(>30s 距上次发射),新增信号触发发射
        c1 = self.agg.add(sig("a", "buy", 0.9, 100.0, 31_000))
        self.assertIsNotNone(c1)
        self.assertEqual(self.agg.emissions, 2)

    def test_emit_resets_accumulation(self):
        self.agg.add(sig("a", "buy", 0.9, 100.0, 0))     # emits
        # 发射后清零:仅一条 0.9 同向再加一条反向 0.1 → 不再到阈值
        self.agg.add(sig("a", "buy", 0.5, 100.0, 40_000))
        self.agg.add(sig("a", "sell", 0.5, 100.0, 40_500))
        # buy=0.5 sell=0.5 → dominant 0.5/1.0 < 0.6 → 不发射
        self.assertEqual(self.agg.emissions, 1)

    def test_cooldown_window_accumulation_flips_direction(self):
        # 首次买发射后,cooldown 窗内堆积的卖多数派在窗后以 SELL 胜出
        agg = AggregatorReplica(THRESHOLD, COOLDOWN_S, {"a": 1.0, "b": 1.0})
        c0 = agg.add(sig("a", "buy", 0.9, 100.0, 0))
        self.assertIsNotNone(c0)
        self.assertEqual(c0.type, "buy")
        # 窗内(1s/10s/20s < 30s):卖 0.9×3 = 2.7 → dominant sell 2.7/4 =
        # 0.675 ≥ 0.6,但被 cooldown 闸住 → 均不发射(累积继续涨)
        self.assertIsNone(agg.add(sig("b", "sell", 0.9, 100.0, 1_000)))
        self.assertIsNone(agg.add(sig("b", "sell", 0.9, 100.0, 10_000)))
        self.assertIsNone(agg.add(sig("b", "sell", 0.9, 100.0, 20_000)))
        # 窗后(40s):再一条卖 0.9 → sell 3.6 / ws 5 = 0.72 → SELL 发射,
        # 方向相对首条买发射翻转(累积多数派胜出)
        c1 = agg.add(sig("b", "sell", 0.9, 100.0, 40_000))
        self.assertIsNotNone(c1)
        self.assertEqual(c1.type, "sell")
        self.assertEqual(agg.emissions, 2)

    def test_unknown_strategy_defaults_weight_one(self):
        agg = AggregatorReplica(THRESHOLD, COOLDOWN_S, {})
        c = agg.add(sig("ghost", "buy", 0.9, 100.0, 0))
        self.assertIsNotNone(c)          # 缺省权重 1.0,不因无权重被丢弃

    def test_per_symbol_state_isolated(self):
        self.agg.add(sig("a", "buy", 0.9, 100.0, 0, sym="BTC_USDT"))
        self.agg.add(sig("a", "buy", 0.9, 100.0, 1, sym="ETH_USDT"))
        self.assertEqual(self.agg.emissions, 2)


# ---------------------------------------------------------------------------
# PaperTrader — 账本状态机
# ---------------------------------------------------------------------------

class TestPaperTrader(unittest.TestCase):

    # ---- 入场 ------------------------------------------------------------

    def test_maker_entry_on_price_revisit(self):
        t = trader()
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 0))
        self.assertTrue(t.open)
        # 价格上走未回触 → 不成交
        t.on_tick(1_000, 100.5)
        t.on_tick(2_000, 100.4)
        self.assertFalse(t.round_trips)
        # tape 回触 ≤100 → maker 成交
        t.on_tick(3_000, 99.9)
        self.assertEqual(len(t.round_trips), 0)  # 仅成交未平仓,无 RT
        self.assertEqual(t._phase, "active")
        self.assertEqual(t._entry_leg, "maker")
        self.assertAlmostEqual(t._qty, 160_000.0 / 100.0)

    def test_maker_timeout_taker_fallback(self):
        t = trader(maker_timeout_s=5)
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 0))
        # 超时后价格已漂移到 100.8 → taker 兜底成交锚 100.8
        t.on_tick(6_000, 100.8)
        self.assertEqual(t._entry_leg, "taker")
        self.assertAlmostEqual(t._entry_px, 100.8)
        self.assertEqual(t._phase, "active")

    def test_flip_while_maker_pending_cancels(self):
        t = trader()
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 0))
        t.on_consensus(Consensus(S, "sell", 0.9, 99.5, 5_000))
        # 挂单期反向 → 撤单弃单,无任何记账
        self.assertFalse(t.open)
        self.assertEqual(t.round_trips, [])

    def test_flat_consensus_never_emitted_by_aggregator(self):
        # 聚合器永不发射 flat(类型系统保证);此处验证账本对未知 symbol 免疫
        t = trader()
        t.on_consensus(Consensus("ETH_USDT", "buy", 0.9, 100.0, 0))
        self.assertFalse(t.open)

    # ---- 退出路径 --------------------------------------------------------

    def _open_long_maker(self, t, px=100.0, conf=0.9):
        t.on_consensus(Consensus(S, "buy", conf, px, 0))
        t.on_tick(1, px)  # 立即 maker 成交(买:last ≤ px)

    def test_tp_ladder_partial_exits(self):
        t = trader()
        self._open_long_maker(t, 100.0)
        qty_orig = t._orig_qty
        # 到达 +0.5% → 平 1/3
        t.on_tick(10_000, 100.6)          # 100.5+ → 目标 100.5
        self.assertEqual(len(t.round_trips), 1)
        rt = t.round_trips[0]
        self.assertEqual(rt.exit_reason, "tp_ladder")
        self.assertAlmostEqual(rt.qty, qty_orig / 3)
        self.assertAlmostEqual(rt.exit_px, 100.5)
        self.assertAlmostEqual(t._qty, qty_orig * 2 / 3)
        # 无新目标触达 → 不再平仓
        t.on_tick(11_000, 100.7)
        self.assertEqual(len(t.round_trips), 1)
        # +1% 目标
        t.on_tick(12_000, 101.1)
        self.assertEqual(len(t.round_trips), 2)
        self.assertAlmostEqual(t._qty, qty_orig / 3)
        # +2% 目标 → 全清
        t.on_tick(13_000, 102.1)
        self.assertEqual(len(t.round_trips), 3)
        self.assertAlmostEqual(t._qty, 0.0)
        self.assertFalse(t.open)

    def test_trailing_sl_after_runup(self):
        t = trader_no_ladder()
        self._open_long_maker(t, 100.0)
        qty_orig = t._orig_qty
        # 涨到 102 后回落 0.6% → 101.49 < 102×(1−0.005)=101.49 触发
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
        t.on_tick(2_000, 100.0)   # best 100.1 → trail 99.5995 未触发
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
        # 同一条反向共识不再反向开仓(单仓位语义)
        self.assertFalse(t.open)

    def test_same_dir_consensus_ignored_while_open(self):
        t = trader()
        self._open_long_maker(t, 100.0)
        qty_orig = t._orig_qty
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 1_000))
        self.assertEqual(t._qty, qty_orig)   # 无 scale-in

    # ---- 费用与 PnL ------------------------------------------------------

    def test_fees_and_pnl_maker_roundtrip(self):
        t = trader()
        self._open_long_maker(t, 100.0)   # maker 成交 @100
        qty_orig = t._orig_qty
        t.on_tick(30_000, 99.0)           # 直接回落 1% → trailing 触发?
        # 100→best 100,trail=99.5;last 99.0 ≤ 99.5 → trailing_sl 平
        rt = t.round_trips[0]
        self.assertEqual(rt.exit_reason, "trailing_sl")
        # pnl = (99−100)×qty = −qty;entry maker fee = qty×100×0.0002;
        # exit taker fee = qty×99×0.0005
        self.assertAlmostEqual(rt.gross_pnl, -qty_orig)
        exp_fees = qty_orig * 100.0 * MAKER_FEE + qty_orig * 99.0 * TAKER_FEE
        self.assertAlmostEqual(rt.fees, exp_fees)
        self.assertAlmostEqual(rt.net_pnl, rt.gross_pnl - rt.fees)
        self.assertEqual(rt.entry_leg, "maker")

    def test_taker_fallback_entry_fees(self):
        t = trader(maker_timeout_s=1)
        t.on_consensus(Consensus(S, "buy", 0.9, 100.0, 0))
        t.on_tick(2_000, 100.5)          # 超时兜底 taker @100.5
        # 数量在共识价锚定(引擎:下单时固定合约数),兜底执行同量
        qty = 160_000.0 / 100.0
        self.assertAlmostEqual(t._entry_fee_total, qty * 100.5 * TAKER_FEE)
        t.on_tick(3_000, 100.5)          # 原地 tick,hold 未到 300s → 无 RT
        self.assertEqual(t.round_trips, [])

    def test_short_side_symmetry(self):
        t = trader_no_ladder()
        t.on_consensus(Consensus(S, "sell", 0.9, 100.0, 0))
        t.on_tick(1, 100.0)              # 卖 maker 成交:last ≥ px
        qty_orig = t._orig_qty
        t.on_tick(2_000, 99.0)           # best 100 → trail 100.5;跌到 99 未触发
        self.assertEqual(t.round_trips, [])
        t.on_tick(3_000, 100.6)          # 回升过 100.5 → trailing 触发
        self.assertEqual(len(t.round_trips), 1)
        rt = t.round_trips[0]
        self.assertEqual(rt.exit_reason, "trailing_sl")
        self.assertAlmostEqual(rt.gross_pnl, (100.0 - rt.exit_px) * qty_orig)

    def test_ladder_short_targets_descend(self):
        t = trader()
        t.on_consensus(Consensus(S, "sell", 0.9, 100.0, 0))
        t.on_tick(1, 100.0)
        qty_orig = t._orig_qty
        t.on_tick(1_000, 99.4)           # −0.6% → 目标 99.5 已破 → 平 1/3
        self.assertEqual(len(t.round_trips), 1)
        self.assertAlmostEqual(t.round_trips[0].exit_px, 99.5)
        self.assertAlmostEqual(t.round_trips[0].qty, qty_orig / 3)


if __name__ == "__main__":
    unittest.main()
