"""Tests for xau.state.positions: PositionBook.mark_seen, forget, sl/tp memo."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from decimal import Decimal

from xau.gate.orders import SIDE_BUY, SIDE_SELL
from xau.state.positions import PositionBook, PositionRecord


@dataclass
class FakePos:
    position_id: int
    side: int
    price: str = "0"
    symbol: str = "XAUUSD"
    time_setup: int = 0


class FakeClock:
    def __init__(self, dt: datetime | None = None) -> None:
        self._now = dt or datetime(2026, 8, 11, 12, 0, 0, tzinfo=timezone.utc)

    def now(self) -> datetime:
        return self._now


def _pos(pid: int, side: int, price: str = "100.00", ts: int = 0) -> FakePos:
    return FakePos(position_id=pid, side=side, price=price, time_setup=ts)


def test_mark_seen_creates_new_records_on_first_sight() -> None:
    book = PositionBook(clock=FakeClock())
    new, all_ = book.mark_seen([_pos(1, SIDE_BUY), _pos(2, SIDE_SELL)])
    assert len(new) == 2
    assert {r.position_id for r in new} == {1, 2}
    assert {r.position_id for r in all_} == {1, 2}
    assert book.records[1].side == SIDE_BUY
    assert book.records[2].side == SIDE_SELL


def test_mark_seen_returns_no_news_on_second_call() -> None:
    book = PositionBook(clock=FakeClock())
    book.mark_seen([_pos(1, SIDE_BUY)])
    new, all_ = book.mark_seen([_pos(1, SIDE_BUY)])
    assert new == []
    assert {r.position_id for r in all_} == {1}


def test_mark_seen_does_not_refresh_existing_entry() -> None:
    """If a position is still there next cycle, its entry price/time
    must NOT be overwritten — those values are anchored to fill time."""
    book = PositionBook(clock=FakeClock())
    book.mark_seen([_pos(1, SIDE_BUY, price="100.00", ts=1000)])
    book.mark_seen([_pos(1, SIDE_BUY, price="200.00", ts=2000)])
    rec = book.records[1]
    assert rec.entry_price == Decimal("100.00")
    assert rec.entry_time == 1000.0


def test_mark_seen_forgets_positions_no_longer_present() -> None:
    book = PositionBook(clock=FakeClock())
    book.mark_seen([_pos(1, SIDE_BUY), _pos(2, SIDE_SELL)])
    book.mark_seen([_pos(1, SIDE_BUY)])  # 2 was closed externally
    assert 2 not in book.records
    assert 1 in book.records


def test_mark_seen_falls_back_to_clock_when_time_setup_zero() -> None:
    clock = FakeClock(datetime(2026, 8, 11, 12, 30, 0, tzinfo=timezone.utc))
    book = PositionBook(clock=clock)
    book.mark_seen([_pos(1, SIDE_BUY, ts=0)])
    expected_ts = datetime(2026, 8, 11, 12, 30, 0, tzinfo=timezone.utc).timestamp()
    assert book.records[1].entry_time == expected_ts


def test_forget_drops_record() -> None:
    book = PositionBook(clock=FakeClock())
    book.mark_seen([_pos(1, SIDE_BUY)])
    book.forget(1)
    assert 1 not in book.records


def test_forget_unknown_id_is_noop() -> None:
    book = PositionBook(clock=FakeClock())
    book.forget(999)  # no error
    assert book.records == {}


def test_should_update_sl_tp_short_circuits_when_equal() -> None:
    book = PositionBook(clock=FakeClock())
    book.mark_seen([_pos(1, SIDE_BUY)])
    book.update_sl_tp(1, sl=Decimal("99.40"), tp=Decimal("100.48"))
    assert not book.should_update_sl_tp(
        book.records[1], sl=Decimal("99.40"), tp=Decimal("100.48"),
    )
    assert book.should_update_sl_tp(
        book.records[1], sl=Decimal("99.50"), tp=Decimal("100.48"),
    )
    assert book.should_update_sl_tp(
        book.records[1], sl=Decimal("99.40"), tp=Decimal("100.50"),
    )


def test_should_update_sl_tp_true_when_never_pushed() -> None:
    book = PositionBook(clock=FakeClock())
    book.mark_seen([_pos(1, SIDE_BUY)])
    assert book.should_update_sl_tp(
        book.records[1], sl=Decimal("99.40"), tp=Decimal("100.48"),
    )


def test_update_sl_tp_preserves_entry_price_and_time() -> None:
    book = PositionBook(clock=FakeClock())
    book.mark_seen([_pos(1, SIDE_BUY, price="100.00", ts=1234)])
    book.update_sl_tp(1, sl=Decimal("99.40"), tp=Decimal("100.48"))
    rec = book.records[1]
    assert rec.entry_price == Decimal("100.00")
    assert rec.entry_time == 1234.0
    assert rec.last_sl == Decimal("99.40")
    assert rec.last_tp == Decimal("100.48")