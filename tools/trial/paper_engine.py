#!/usr/bin/env python3
"""paper_engine.py — A/B 权重纸面实验核心(纯逻辑,无 DB 依赖)

镜像 pulseTrader 引擎语义,用于在信号日志上离线重放任意权重策略:

  1. AggregatorReplica — SignalAggregator.cpp 逐行复刻:
     - Flat 信号不参与累积
     - 每 symbol 累积 buy/sell 加权置信度与 weight_sum
     - normalized = dominant/weight_sum < threshold → 不发射
     - 距上次发射 < cooldown → 不发射(C++ 顺序:先 threshold 后
       cooldown;被闸时累积不清零、继续增长)
     - 发射后清零并记 last_signal;时钟 = 信号流时间(重放时钟)
     - 未注册策略权重 = 1.0(C++ getWeight 缺省)

  2. PaperTrader — symbol 级单仓位纸面成交状态机:
     phase: None → maker(挂单等触价)→ active(持仓监控)→ None
     - 入场 maker-first:限价 P=共识价挂着;tape 回触(买:last ≤ P;
       卖:last ≥ P)即 maker 成交;超 maker_timeout_s 未触 → taker
       市价按当时 last 兜底(引擎 M16 maker-first 语义)
     - maker 挂单期遇反向共识 → 撤单弃单(无成交不记账)
     - active 期逐 tick 镜像 StopLossEngine/TakeProfitEngine:
       * TP 阶梯:last 达 targets_pct[i] → 平 fractions[i]×原始量(taker)
       * Trailing SL:best 跟踪;买:last ≤ best×(1−trailing_pct) →
         平剩余(taker)
       * max_hold_s 超时 → 平剩余(taker)
       * 反向共识 → 平剩余(taker);该信号不再反向开新仓
     - 同向共识在持仓/挂单期忽略(单仓位,不 scale-in)
     - 费用:maker 腿 maker_fee、其余 taker_fee;入场费按平仓量比例
       分摊到各 RoundTrip(阶梯每腿 + 剩余清仓)

时间戳统一 ns(与 signals.ts_ns 一致);tick 输入 (ts_ms, last)。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


# ---------------------------------------------------------------------------
# 信号与聚合器复刻
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
    """聚合器发射的共识信号(镜像 TradingSignal)。"""
    symbol: str
    type: str                 # "buy" / "sell"(永不 flat)
    confidence: float
    price: float
    ts_ns: int


class AggregatorReplica:
    """SignalAggregator.cpp 纯函数复刻。按 symbol 分状态;add 返回
    发射的 Consensus(至多一条)或 None。重放时钟 = 信号自身时间。"""

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

        # 1. threshold 闸门(与 C++ 顺序一致)
        if normalized < self.threshold:
            return None
        # 2. cooldown 闸门 — 被闸时累积不清零,继续增长(C++ 语义)。
        #    缺省 last_emit=None 表示从未发射(重放时钟可能从 0 起,
        #    不能用 0 作哨兵 — C++ 用墙钟 nowMs() 所以无此问题)。
        last_emit = self._last_emit_ns.get(sym)
        if last_emit is not None \
                and now_ns - last_emit < self.cooldown_ns:
            return None

        # 3. 发射 + 清零
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
# 纸面账本
# ---------------------------------------------------------------------------

@dataclass
class RoundTrip:
    entry_ts_ns: int
    exit_ts_ns: int
    direction: str              # "buy"(long) / "sell"(short)
    entry_px: float
    entry_leg: str              # "maker" / "taker"
    exit_px: float
    exit_reason: str            # "tp_ladder" / "trailing_sl" / "max_hold" / "flip"
    qty: float
    gross_pnl: float
    fees: float                 # 入场费分摊 + 出场费
    net_pnl: float
    notional: float
    conf_at_entry: float
    held_ms: int


class PaperTrader:
    """symbol 级纸面成交状态机。事件接口:on_consensus / on_tick。"""

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
        self._qty: float = 0.0           # 剩余数量
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
            # 挂单期反向共识 → 撤单弃单(未成交不记账)
            if c.type != self._dir:
                self._clear_position()
        else:  # active
            if c.type != self._dir:
                self._book_close(c.price, "flip", c.ts_ns)

    def on_tick(self, ts_ms: int, last: float):
        """逐 tick 评估:maker 触价、taker 兜底、TP 阶梯、trailing、max_hold。"""
        ts_ns = ts_ms * 1_000_000
        if self._phase == "maker":
            if self._dir == "buy" and last <= self._entry_px:
                self._establish(ts_ns, self._entry_px, "maker")
            elif self._dir == "sell" and last >= self._entry_px:
                self._establish(ts_ns, self._entry_px, "maker")
            elif ts_ns >= self._maker_deadline_ns:
                # 超时 → taker 市价兜底(引擎 M16:超时后市价补单)
                self._establish(ts_ns, last, "taker")
        elif self._phase == "active":
            self._evaluate_exits(ts_ns, last)

    # ---- internal ---------------------------------------------------------

    def _establish(self, ts_ns: int, fill_px: float, leg: str):
        """挂单成交/兜底 → active。入场价锚定实际成交价。"""
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
            # TP 阶梯(按目标价递增逐级触发)
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
        """记一条平仓腿(阶梯或最终清仓共用),按 qty 比例分摊入场费。"""
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
        # 阶梯份额总和 = 1.0 时最后一腿平完即清仓;1/3×3 浮点残差
        # ~1e-9 相对量级,用相对 epsilon 判定
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
