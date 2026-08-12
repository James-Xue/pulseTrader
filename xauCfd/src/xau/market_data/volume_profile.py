"""Rolling volume profile — bars → binned time-at-price → top-N nodes.

Construction:
  * Each synthetic `Kline` is sampled into `bin_usd`-wide price buckets.
    The "volume" of a bucket is the **time the market spent in that bucket**
    during the bar (i.e. `duration_s * (1 / tick_count)` per tick, summed
    across ticks). When `tick_count == 1` we cannot infer intra-bar time
    distribution, so we approximate by assigning the full `duration_s` to
    the single tick's price bucket.
  * A rolling deque keeps the most recent `window_bars` bars. Older bars
    are evicted FIFO.
  * `top_nodes(n, min_sep_pts)` runs a non-maximum-suppression pass:
    greedy, descending by bin-volume, skipping any candidate within
    `min_sep_pts` of an already-selected higher-volume node.

This module has no side effects. The caller (`apps.daemon`) feeds bars in
and reads nodes out; the profile is purely a data structure.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from decimal import Decimal


@dataclass(frozen=True, slots=True)
class Node:
    """One extracted volume peak. `price` is the bin center."""
    price: Decimal
    seconds: float  # total time-at-price across the window


@dataclass(slots=True)
class VolumeProfile:
    """Rolling time-at-price profile over a window of synthetic bars.

    `bin_usd` defaults to 0.10 (10 points) — matches XAU's minimum
    meaningful granularity on the broker side (price_precision=2 means the
    server rounds to cents, so a 10-pt bin is 5 cents wide which is still
    much coarser than the wire but enough for S/R detection).

    `window_bars` defaults to 200 (~33 min at 10s bars).
    """

    bin_usd: Decimal = Decimal("0.10")
    window_bars: int = 200
    _bins: dict[int, float] = field(default_factory=dict)
    _bar_count: int = 0
    _bars: deque = field(default_factory=lambda: deque(maxlen=200))

    def __post_init__(self) -> None:
        if self.bin_usd <= 0:
            raise ValueError(f"bin_usd must be positive, got {self.bin_usd}")
        if self.window_bars <= 0:
            raise ValueError(f"window_bars must be positive, got {self.window_bars}")
        # Resize the deque to match the configured window. The lambda default
        # captured `200` literally, so we rebuild it here.
        if self._bars.maxlen != self.window_bars:
            old = list(self._bars)
            self._bars = deque(old, maxlen=self.window_bars)

    @property
    def bar_count(self) -> int:
        return self._bar_count

    @property
    def total_seconds(self) -> float:
        return sum(self._bins.values())

    def _bin_index(self, price: Decimal) -> int:
        """Floor a price to its bin index. Bin `i` covers `[i*bin_usd, (i+1)*bin_usd)`."""
        # Use integer math to avoid Decimal/float drift.
        scaled = int(price / self.bin_usd)
        return scaled

    def _bin_center(self, idx: int) -> Decimal:
        return self.bin_usd * idx

    def _distribute_bar_seconds(self, bar) -> None:
        """Add the bar's duration into bins according to its OHLC.

        Without intra-bar data we approximate by weighting each of O/H/L/C
        by 0.25 of the bar's duration. A future iteration that has true
        intra-bar ticks can replace this with a uniform distribution across
        the [low, high] range.
        """
        if bar.duration_s <= 0 or bar.tick_count == 0:
            return
        if bar.tick_count == 1:
            # Single tick — assign full duration to the close price's bin.
            self._bins[self._bin_index(bar.close)] = (
                self._bins.get(self._bin_index(bar.close), 0.0) + bar.duration_s
            )
            return
        quarter = bar.duration_s / 4.0
        for px in (bar.open, bar.high, bar.low, bar.close):
            idx = self._bin_index(px)
            self._bins[idx] = self._bins.get(idx, 0.0) + quarter

    def add_bar(self, bar) -> None:
        """Roll a new bar into the profile; evict the oldest if at capacity."""
        self._bars.append(bar)
        # When the deque evicts an old bar, we need to subtract its contribution.
        # But we distribute per-tick on add (no per-tick history kept), so we
        # approximate eviction by subtracting on a best-effort basis: only if
        # the bar we are evicting is the same one we'd reproduce now. Since
        # we don't store that, we keep the bins additive and let `total_seconds`
        # drift up. To prevent unbounded growth, we recompute from the live
        # `_bars` deque on every add. This is O(window) per add but window
        # is small (200) so it's fine.
        self._rebuild_from_bars()

    def _rebuild_from_bars(self) -> None:
        """Recompute the bin dict from the current `_bars` deque."""
        self._bins.clear()
        for bar in self._bars:
            self._distribute_bar_seconds(bar)
        self._bar_count = len(self._bars)

    def top_nodes(self, n: int, min_sep_pts: float) -> list[Node]:
        """Return up to `n` peaks by descending bin volume, NMS with `min_sep_pts`.

        `min_sep_pts` is expressed in price POINTS (1 pt = $0.01). The
        internal bin width is `bin_usd * 100` points. A `min_sep_pts` of 50
        (= $0.50) means peaks must be at least 5 bins apart.
        """
        if n <= 0:
            return []
        if min_sep_pts < 0:
            raise ValueError(f"min_sep_pts must be non-negative, got {min_sep_pts}")
        # Sort bins by seconds descending.
        sorted_bins = sorted(self._bins.items(), key=lambda kv: kv[1], reverse=True)
        min_sep_bins = max(1, int(min_sep_pts / (self.bin_usd * 100)))
        selected: list[Node] = []
        for idx, secs in sorted_bins:
            if secs <= 0:
                continue
            cand_center = self._bin_center(idx)
            # Skip if within min_sep_bins of an already-selected node.
            too_close = False
            for existing in selected:
                diff = abs(cand_center - existing.price)
                if diff <= self.bin_usd * min_sep_bins:
                    too_close = True
                    break
            if too_close:
                continue
            selected.append(Node(price=cand_center, seconds=secs))
            if len(selected) >= n:
                break
        return selected

    def median_seconds(self) -> float:
        """Median of non-zero bin volumes — used as a noise floor filter."""
        non_zero = [v for v in self._bins.values() if v > 0]
        if not non_zero:
            return 0.0
        non_zero.sort()
        mid = len(non_zero) // 2
        if len(non_zero) % 2:
            return non_zero[mid]
        return (non_zero[mid - 1] + non_zero[mid]) / 2.0

    def filter_by_min_seconds(self, min_seconds: float) -> None:
        """Drop bins whose seconds are below `min_seconds` (e.g. < 2.5× median)."""
        if min_seconds < 0:
            raise ValueError(f"min_seconds must be non-negative, got {min_seconds}")
        self._bins = {k: v for k, v in self._bins.items() if v >= min_seconds}