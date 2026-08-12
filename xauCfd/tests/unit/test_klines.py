"""Tests for the synthetic BarAggregator."""
from __future__ import annotations

from decimal import Decimal

from xau.market_data.klines import BarAggregator, Kline, Tick


def _tick(ts: float, price: str) -> Tick:
    p = Decimal(price)
    return Tick(ts=ts, price=p, bid=p - Decimal("0.05"), ask=p + Decimal("0.05"))


def test_first_tick_returns_none_no_close() -> None:
    agg = BarAggregator(bucket_s=10)
    assert agg.add(_tick(1000.0, "100")) is None
    # flush() will close the partial bucket containing the single tick.
    flushed = agg.flush(1005.0)
    assert flushed is not None
    assert flushed.tick_count == 1
    assert flushed.open_ts == 1000.0
    assert flushed.close_ts == 1005.0


def test_same_bucket_returns_none() -> None:
    agg = BarAggregator(bucket_s=10)
    agg.add(_tick(1000.0, "100"))  # bucket [1000, 1010)
    assert agg.add(_tick(1005.0, "101")) is None
    assert agg.add(_tick(1009.9, "102")) is None


def test_rollover_emits_closed_bar_with_ohlc() -> None:
    agg = BarAggregator(bucket_s=10)
    agg.add(_tick(1000.0, "100"))
    agg.add(_tick(1005.0, "110"))  # high
    agg.add(_tick(1008.0, "95"))   # low (and the last tick in the old bucket)
    closed = agg.add(_tick(1011.0, "105"))  # crosses into bucket [1010, 1020)
    assert closed is not None
    assert isinstance(closed, Kline)
    assert closed.open_ts == 1000.0
    assert closed.open == Decimal("100")
    assert closed.high == Decimal("110")
    assert closed.low == Decimal("95")
    # Close = last tick in the OLD bucket (95), not the rollover tick (105)
    assert closed.close == Decimal("95")
    assert closed.tick_count == 3
    assert closed.duration_s == 11.0  # from bucket start (1000) to rollover tick (1011)


def test_new_bar_starts_at_rollover_tick() -> None:
    agg = BarAggregator(bucket_s=10)
    agg.add(_tick(1000.0, "100"))
    rollover = agg.add(_tick(1011.0, "120"))  # rollover — closes first bar
    assert rollover is not None
    assert rollover.open_ts == 1000.0
    assert rollover.close == Decimal("100")  # last tick in old bucket
    # Second tick in the new bucket.
    assert agg.add(_tick(1015.0, "125")) is None
    closed = agg.add(_tick(1021.0, "115"))
    assert closed is not None
    assert closed.open_ts == 1010.0  # new bucket starts at floor(1011/10)*10
    assert closed.open == Decimal("120")
    assert closed.high == Decimal("125")
    assert closed.low == Decimal("120")
    # Close = last tick in this bucket (125 at 1015), not the rollover tick (115)
    assert closed.close == Decimal("125")
    assert closed.tick_count == 2


def test_flush_returns_partial_bucket() -> None:
    agg = BarAggregator(bucket_s=10)
    agg.add(_tick(1000.0, "100"))
    agg.add(_tick(1005.0, "110"))
    flushed = agg.flush(1007.0)
    assert flushed is not None
    assert flushed.open_ts == 1000.0
    assert flushed.close_ts == 1007.0
    assert flushed.tick_count == 2


def test_flush_when_empty_returns_none() -> None:
    agg = BarAggregator(bucket_s=10)
    assert agg.flush(999.0) is None


def test_flush_resets_state() -> None:
    agg = BarAggregator(bucket_s=10)
    agg.add(_tick(1000.0, "100"))
    agg.flush(1005.0)
    # After flush, the next tick starts a fresh bucket.
    assert agg.add(_tick(1020.0, "200")) is None
    closed = agg.add(_tick(1031.0, "210"))
    assert closed is not None
    assert closed.open == Decimal("200")  # confirms state was reset


def test_invalid_bucket_s_rejected() -> None:
    import pytest
    with pytest.raises(ValueError, match="bucket_s must be positive"):
        BarAggregator(bucket_s=0)
    with pytest.raises(ValueError, match="bucket_s must be positive"):
        BarAggregator(bucket_s=-5)


def test_bucket_s_property() -> None:
    agg = BarAggregator(bucket_s=15)
    assert agg.bucket_s == 15


def test_single_tick_ohlc_equals_price() -> None:
    agg = BarAggregator(bucket_s=10)
    agg.add(_tick(1000.0, "1234.50"))
    closed = agg.add(_tick(1011.0, "1234.50"))
    assert closed is not None
    assert closed.open == closed.high == closed.low == closed.close == Decimal("1234.50")


def test_high_low_tracking_with_equal_prices() -> None:
    agg = BarAggregator(bucket_s=10)
    agg.add(_tick(1000.0, "100"))
    agg.add(_tick(1001.0, "100"))  # flat
    agg.add(_tick(1002.0, "100"))
    closed = agg.add(_tick(1011.0, "100"))
    assert closed is not None
    assert closed.high == closed.low == Decimal("100")
    assert closed.tick_count == 3


def test_tick_uses_close_price_for_continuation() -> None:
    """When the rollover happens, the new bar's open is the rollover tick's price."""
    agg = BarAggregator(bucket_s=10)
    agg.add(_tick(1000.0, "100"))
    agg.add(_tick(1005.0, "105"))
    rollover_tick = _tick(1011.0, "107.5")
    closed = agg.add(rollover_tick)
    assert closed is not None
    # Closed bar = (100, 105, 100, 105) — close is the last tick in OLD bucket.
    assert closed.close == Decimal("105")
    # Next rollover reveals the new bar started at 107.5
    next_closed = agg.add(_tick(1021.0, "108"))
    assert next_closed is not None
    assert next_closed.open == Decimal("107.5")