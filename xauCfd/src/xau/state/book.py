"""Local book — open positions and pending orders, with diff helpers.

The daemon polls `list_open_pendings` each cycle and gets back a snapshot
of what's currently on the broker. It also computes a fresh `plan`. The
diff between (plan) and (actual) is what gets applied.

Actions:
  * PLACE(level_price, side, price, lot) — broker doesn't have this limit
  * CANCEL(order_id) — plan no longer includes this limit (price moved,
    level evicted from top-N, trend flipped, etc.)

The Book is a plain dataclass container — no mutation logic on its own.
The diff helpers return lists of `Action` that the daemon then executes
(or just logs in dry-run mode).
"""
from __future__ import annotations

from dataclasses import dataclass, field
from decimal import Decimal

from xau.gate.orders import Order, SIDE_BUY, SIDE_SELL
from xau.strategy.sr_reversion import PlanItem, Side


@dataclass(frozen=True, slots=True)
class Action:
    """One order-management action: either place a new limit or cancel an existing one."""
    kind: str  # "PLACE" or "CANCEL"
    symbol: str
    side: int | None = None        # for PLACE — 2=buy, 1=sell (per official CFD schema)
    price: Decimal | None = None   # for PLACE
    lot: Decimal | None = None     # for PLACE
    level_price: Decimal | None = None  # for PLACE — for the log/audit
    order_id: int | None = None    # for CANCEL
    reason: str = ""


@dataclass(slots=True)
class Book:
    """Snapshot of broker state — populated each cycle from list_* REST calls."""
    pendings: list[Order] = field(default_factory=list)
    positions: list = field(default_factory=list)  # list[Position] — local import only

    def diff_against_plan(
        self,
        plan: list[PlanItem],
        *,
        price_tolerance_pts: float = 5.0,
    ) -> list[Action]:
        """Compute the actions needed to make `self.pendings` match `plan`.

        PLACE: a plan item that doesn't match any existing pending
          (matched by symbol + side + price within `price_tolerance_pts`).
        CANCEL: a pending that doesn't correspond to any plan item.

        `price_tolerance_pts` lets us consider "the same limit" if the
        price drifted by a few points between cycles (e.g. new bar data
        re-anchored the level). 5 pts = $0.05 — well inside the broker's
        minimum price increment.

        Returned list is ordered: CANCELs first, PLACEs second. Cancels
        release margin before we add new exposure.
        """
        actions: list[Action] = []
        one_pt = Decimal("0.01")
        tol = one_pt * int(price_tolerance_pts)

        # Linear scan: for each plan item, find the closest pending on the
        # same side+symbol within `tol`. O(N*M) but N, M are tiny (≤6 each).
        def find_match(item: PlanItem) -> Order | None:
            best: Order | None = None
            best_diff: Decimal | None = None
            for o in self.pendings:
                if o.symbol != item.symbol:
                    continue
                # SIDE_BUY=2, SIDE_SELL=1 in orders module (per official CFD
                # schema); map plan side string to that int.
                plan_side_int = 2 if item.side.value == "BUY" else 1
                if int(getattr(o, "side", 0)) != plan_side_int:
                    continue
                try:
                    px = Decimal(o.price)
                except Exception:
                    continue
                diff = abs(px - item.price)
                if diff <= tol and (best_diff is None or diff < best_diff):
                    best = o
                    best_diff = diff
            return best

        matched_order_ids: set[int] = set()
        for item in plan:
            existing = find_match(item)
            if existing is not None and existing.order_id not in matched_order_ids:
                matched_order_ids.add(existing.order_id)
                continue
            actions.append(Action(
                kind="PLACE",
                symbol=item.symbol,
                side=SIDE_BUY if item.side == Side.BUY else SIDE_SELL,
                price=item.price,
                lot=item.lot,
                level_price=item.level_price,
                reason=item.reason,
            ))

        # Cancel anything in the book not matched by the plan.
        for o in self.pendings:
            if o.order_id not in matched_order_ids:
                actions.append(Action(
                    kind="CANCEL",
                    symbol=o.symbol,
                    order_id=o.order_id,
                    reason=f"pending {o.order_id} no longer in plan",
                ))

        # Re-order: CANCELs first.
        actions.sort(key=lambda a: 0 if a.kind == "CANCEL" else 1)
        return actions


def orphan_position_ids(positions: list, plan_sides: set[int]) -> list[int]:
    """Return position IDs whose side is not present in the active plan.

    The plan is a list of pending-order intents; an "orphan" position is
    one whose direction the planner no longer wants (e.g., trend flipped
    to DOWN, current plan only emits BUY_LIMITs, so any existing Short
    position should be closed).

    `positions` is list[xau.gate.positions.Position]. We only touch the
    `position_id` and `side` fields here; the type is left duck-typed so
    this module doesn't import from gate/.
    """
    return [p.position_id for p in positions if p.side not in plan_sides]