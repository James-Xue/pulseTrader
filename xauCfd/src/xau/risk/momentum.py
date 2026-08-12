"""Momentum-based take-profit policy.

User rule (2026-08-11): "根据反弹动量决定是否止盈".
Concretely: if the bid/ask reverses AGAINST a fresh position by at least
`reversal_pts` within `window_s` seconds of fill, close the position.
Default: 0.48 pts (= 0.8 × sl_usd) within 60 s.

The reversal threshold is intentionally below the SL distance to capture
weak bounces before they develop into full SL hits. Outside the time
window the policy stays silent — the broker-level SL (if any) is the only
protection.
"""
from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal

from xau.gate.orders import SIDE_BUY, SIDE_SELL
from xau.state.positions import PositionRecord


@dataclass(frozen=True, slots=True)
class MomentumTpPolicy:
    """Reversal-threshold + time-window policy."""
    reversal_pts: Decimal   # e.g. Decimal("0.48")
    window_s: int           # e.g. 60

    def should_take_profit(
        self,
        record: PositionRecord,
        *,
        now: float,
        bid: Decimal,
        ask: Decimal,
    ) -> bool:
        """True if adverse move ≥ `reversal_pts` within `window_s` of fill.

        Adverse move direction:
          * BUY (Long) — the bid falling below entry is adverse.
          * SELL (Short) — the ask rising above entry is adverse.

        Outside the time window, returns False — broker SL is the only
        protection in that regime.
        """
        if (now - record.entry_time) > self.window_s:
            return False
        if record.side == SIDE_BUY:
            adverse = record.entry_price - bid
        elif record.side == SIDE_SELL:
            adverse = ask - record.entry_price
        else:
            return False
        return adverse >= self.reversal_pts