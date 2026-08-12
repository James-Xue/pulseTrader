"""Synthetic 10-second bars built from ticker polling.

Since the Gate.io TradFi API does NOT expose historical klines for XAUUSD
(both `/tradfi/symbols/XAUUSD/klines` REST and the WS candle channels are
non-functional — see market_data/__init__.py), we synthesize bars locally:
the daemon polls the ticker every `poll_ms`, and `BarAggregator` rolls up
those ticks into fixed-width `Kline` buckets of `bucket_s` seconds.

These bars carry NO real volume data — they have only price action and
tick_count. The downstream volume profile therefore uses **time-at-price**
(seconds the last_price spent in each $0.10 bin) as a proxy for liquidity.
This is sufficient for mean-reversion: a level where the market rested for
many seconds is more meaningful than one with a single transient touch.

Design:
  * `TickSource` protocol: anything that yields `Tick` objects on demand.
    In production: a polling loop around `fetch_ticker`. In tests: an
    in-memory list.
  * `BarAggregator` is stateless across buckets — it accumulates the
    current bucket, and `close_bucket()` returns a finished `Kline` when
    the bucket period elapses.
"""
from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from typing import Iterator, Protocol


@dataclass(frozen=True, slots=True)
class Tick:
    """One price observation. `ts` is wall-clock UNIX seconds."""
    ts: float
    price: Decimal
    bid: Decimal
    ask: Decimal


@dataclass(frozen=True, slots=True)
class Kline:
    """One synthetic 10-second OHLC bucket.

    All prices are Decimal. `tick_count` is the number of ticks aggregated.
    `duration_s` is typically `bucket_s` but may be shorter for the first
    and last buckets of a session.
    """
    open_ts: float
    close_ts: float
    open: Decimal
    high: Decimal
    low: Decimal
    close: Decimal
    tick_count: int
    duration_s: float


class TickSource(Protocol):
    """Anything that yields Tick objects. Used by the daemon's poll loop."""

    def __iter__(self) -> Iterator[Tick]: ...


class BarAggregator:
    """Roll ticks into fixed-width OHLC buckets.

    Usage:
        agg = BarAggregator(bucket_s=10)
        for tick in source:
            bar = agg.add(tick)
            if bar is not None:
                # bar is a closed Kline; feed to volume profile, etc.

    `add(tick)` returns a `Kline` whenever the new tick crosses a bucket
    boundary. The returned bar is the JUST-CLOSED bucket; the new tick
    begins the next bucket. Returns None if the tick is still inside the
    current bucket.
    """

    def __init__(self, bucket_s: float = 10.0) -> None:
        if bucket_s <= 0:
            raise ValueError(f"bucket_s must be positive, got {bucket_s}")
        self._bucket_s = bucket_s
        self._bucket_open_ts: float | None = None
        self._o: Decimal | None = None
        self._h: Decimal | None = None
        self._l: Decimal | None = None
        self._c: Decimal | None = None
        self._n: int = 0

    @property
    def bucket_s(self) -> float:
        return self._bucket_s

    def _bucket_start(self, ts: float) -> float:
        """Floor `ts` to the nearest bucket boundary."""
        import math
        return math.floor(ts / self._bucket_s) * self._bucket_s

    def add(self, tick: Tick) -> Kline | None:
        """Add a tick. Returns the just-closed bar if a bucket rolled over, else None."""
        bucket_start = self._bucket_start(tick.ts)
        if self._bucket_open_ts is None:
            # First tick ever.
            self._bucket_open_ts = bucket_start
            self._o = self._h = self._l = self._c = tick.price
            self._n = 1
            return None

        if bucket_start == self._bucket_open_ts:
            # Same bucket.
            assert self._h is not None and self._l is not None
            if tick.price > self._h:
                self._h = tick.price
            if tick.price < self._l:
                self._l = tick.price
            self._c = tick.price
            self._n += 1
            return None

        # Bucket rolled over — close the old bucket, start a new one.
        assert self._o is not None and self._h is not None
        assert self._l is not None and self._c is not None
        closed = Kline(
            open_ts=self._bucket_open_ts,
            close_ts=tick.ts,
            open=self._o,
            high=self._h,
            low=self._l,
            close=self._c,
            tick_count=self._n,
            duration_s=tick.ts - self._bucket_open_ts,
        )
        self._bucket_open_ts = bucket_start
        self._o = self._h = self._l = self._c = tick.price
        self._n = 1
        return closed

    def flush(self, now_ts: float) -> Kline | None:
        """Force-close the current bucket (e.g. on daemon shutdown)."""
        if self._bucket_open_ts is None or self._n == 0:
            return None
        assert self._o is not None and self._h is not None
        assert self._l is not None and self._c is not None
        closed = Kline(
            open_ts=self._bucket_open_ts,
            close_ts=now_ts,
            open=self._o,
            high=self._h,
            low=self._l,
            close=self._c,
            tick_count=self._n,
            duration_s=now_ts - self._bucket_open_ts,
        )
        self._bucket_open_ts = None
        self._o = self._h = self._l = self._c = None
        self._n = 0
        return closed