"""Tests for state.book: Book.diff_against_plan."""
from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from typing import Any

from xau.gate.orders import SIDE_BUY, SIDE_SELL
from xau.state.book import Action, Book
from xau.strategy.sr_reversion import PlanItem, Side


@dataclass
class FakeOrder:
    order_id: int
    symbol: str
    side: int
    price: str
    state: int = 1
    finished: int = 0
    leverage: str = "500"
    price_type: str = "trigger"
    price_tp: str = "0"
    price_sl: str = "0"
    time_setup: int = 0


def _plan_item(*, side: Side, price: str, lot: str = "0.01",
               level_price: str | None = None) -> PlanItem:
    return PlanItem(
        symbol="XAUUSD",
        side=side,
        price=Decimal(price),
        lot=Decimal(lot),
        level_price=Decimal(level_price or price),
        confidence=0.5,
        reason="test",
    )


def test_empty_book_empty_plan_no_actions() -> None:
    book = Book()
    actions = book.diff_against_plan([])
    assert actions == []


def test_plan_with_no_existing_pendings_places_all() -> None:
    book = Book(pendings=[])
    plan = [_plan_item(side=Side.BUY, price="4367.50"),
            _plan_item(side=Side.SELL, price="4368.50")]
    actions = book.diff_against_plan(plan)
    assert len(actions) == 2
    assert all(a.kind == "PLACE" for a in actions)


def test_plan_matches_existing_pending_no_actions() -> None:
    """Same plan and book → no actions."""
    existing = FakeOrder(order_id=100, symbol="XAUUSD", side=SIDE_BUY, price="4367.50")
    book = Book(pendings=[existing])
    plan = [_plan_item(side=Side.BUY, price="4367.50")]
    actions = book.diff_against_plan(plan)
    assert actions == []


def test_price_within_tolerance_matches() -> None:
    """If price drifted 3 pts, still considered the same pending (tol=5)."""
    existing = FakeOrder(order_id=100, symbol="XAUUSD", side=SIDE_BUY, price="4367.50")
    book = Book(pendings=[existing])
    plan = [_plan_item(side=Side.BUY, price="4367.47")]  # 3 pts off
    actions = book.diff_against_plan(plan, price_tolerance_pts=5.0)
    assert actions == []


def test_price_outside_tolerance_places_new() -> None:
    """If price drifted >5 pts, place new + cancel old."""
    existing = FakeOrder(order_id=100, symbol="XAUUSD", side=SIDE_BUY, price="4367.50")
    book = Book(pendings=[existing])
    plan = [_plan_item(side=Side.BUY, price="4367.30")]  # 20 pts off
    actions = book.diff_against_plan(plan, price_tolerance_pts=5.0)
    kinds = [a.kind for a in actions]
    assert "PLACE" in kinds and "CANCEL" in kinds


def test_pending_not_in_plan_is_cancelled() -> None:
    """If the plan no longer needs this pending, cancel it."""
    existing = FakeOrder(order_id=100, symbol="XAUUSD", side=SIDE_BUY, price="4367.50")
    book = Book(pendings=[existing])
    plan = [_plan_item(side=Side.SELL, price="4368.50")]  # different level
    actions = book.diff_against_plan(plan)
    assert len(actions) == 2
    cancel = [a for a in actions if a.kind == "CANCEL"][0]
    assert cancel.order_id == 100


def test_actions_ordered_cancels_before_places() -> None:
    existing1 = FakeOrder(order_id=1, symbol="XAUUSD", side=SIDE_BUY, price="4367.50")
    existing2 = FakeOrder(order_id=2, symbol="XAUUSD", side=SIDE_SELL, price="4368.50")
    book = Book(pendings=[existing1, existing2])
    plan = [_plan_item(side=Side.BUY, price="4366.00"),  # neither matches
            _plan_item(side=Side.SELL, price="4369.00")]
    actions = book.diff_against_plan(plan)
    # All 4 actions (2 cancels + 2 places) — cancels should come first.
    cancels = [i for i, a in enumerate(actions) if a.kind == "CANCEL"]
    places = [i for i, a in enumerate(actions) if a.kind == "PLACE"]
    assert max(cancels) < min(places)


def test_action_includes_level_price_for_audit() -> None:
    book = Book(pendings=[])
    plan = [_plan_item(side=Side.BUY, price="4367.50", level_price="4367.80")]
    actions = book.diff_against_plan(plan)
    assert isinstance(actions[0], Action)
    assert actions[0].level_price == Decimal("4367.80")
    assert actions[0].price == Decimal("4367.50")


def test_action_carries_reason() -> None:
    book = Book(pendings=[])
    plan = [_plan_item(side=Side.BUY, price="4367.50")]
    actions = book.diff_against_plan(plan)
    assert actions[0].reason == "test"


def test_mixed_match_place_and_cancel() -> None:
    """3 in plan, 2 existing — 1 matches, 1 place, 1 cancel."""
    existing_a = FakeOrder(order_id=1, symbol="XAUUSD", side=SIDE_BUY, price="4367.50")  # matches
    existing_b = FakeOrder(order_id=2, symbol="XAUUSD", side=SIDE_SELL, price="4365.00")  # orphan
    book = Book(pendings=[existing_a, existing_b])
    plan = [
        _plan_item(side=Side.BUY, price="4367.50"),   # matches existing_a
        _plan_item(side=Side.SELL, price="4369.00"),  # new
    ]
    actions = book.diff_against_plan(plan)
    kinds = sorted(a.kind for a in actions)
    assert kinds == ["CANCEL", "PLACE"]
    cancel = next(a for a in actions if a.kind == "CANCEL")
    place = next(a for a in actions if a.kind == "PLACE")
    assert cancel.order_id == 2
    assert place.price == Decimal("4369.00")


def test_orphan_position_ids_drops_matching_side() -> None:
    """Positions whose side IS in the plan are kept; others are orphan IDs."""
    from xau.state.book import orphan_position_ids

    @dataclass
    class FakePos:
        position_id: int
        side: int

    positions = [FakePos(position_id=10, side=SIDE_BUY),
                 FakePos(position_id=20, side=SIDE_SELL)]
    plan_sides = {SIDE_BUY}  # only BUY in plan → Short is orphan
    orphans = orphan_position_ids(positions, plan_sides)
    assert orphans == [20]


def test_orphan_position_ids_all_kept_when_sides_match() -> None:
    from xau.state.book import orphan_position_ids

    @dataclass
    class FakePos:
        position_id: int
        side: int

    positions = [FakePos(position_id=10, side=SIDE_BUY),
                 FakePos(position_id=20, side=SIDE_SELL)]
    plan_sides = {SIDE_BUY, SIDE_SELL}
    assert orphan_position_ids(positions, plan_sides) == []


def test_orphan_position_ids_all_closed_when_plan_empty() -> None:
    from xau.state.book import orphan_position_ids

    @dataclass
    class FakePos:
        position_id: int
        side: int

    positions = [FakePos(position_id=10, side=SIDE_BUY),
                 FakePos(position_id=20, side=SIDE_SELL)]
    assert orphan_position_ids(positions, set()) == [10, 20]