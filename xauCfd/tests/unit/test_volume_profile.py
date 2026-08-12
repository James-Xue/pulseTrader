"""Tests for VolumeProfile: bin assignment, window eviction, top_nodes NMS."""
from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal

from xau.market_data.volume_profile import Node, VolumeProfile


def _bar(open: str, high: str, low: str, close: str, *, tick_count: int = 4, duration_s: float = 10.0):
    return _Bar(
        open=Decimal(open), high=Decimal(high), low=Decimal(low), close=Decimal(close),
        tick_count=tick_count, duration_s=duration_s,
    )


@dataclass
class _Bar:
    open: Decimal
    high: Decimal
    low: Decimal
    close: Decimal
    tick_count: int
    duration_s: float


def test_empty_profile_has_no_nodes() -> None:
    p = VolumeProfile()
    assert p.bar_count == 0
    assert p.total_seconds == 0.0
    assert p.top_nodes(n=6, min_sep_pts=50) == []


def test_single_bar_distributes_seconds_across_ohlc() -> None:
    """A 10s bar with 4 ticks splits its time 2.5s to O/H/L/C bins."""
    p = VolumeProfile(bin_usd=Decimal("0.10"))
    p.add_bar(_bar("100.00", "101.00", "99.00", "100.50"))
    # Total seconds = 10.0 (4 ticks, equal share 2.5s each).
    assert p.total_seconds == 10.0
    # The bins touched: 99.00 (idx 990), 100.00 (idx 1000), 100.50 (idx 1005),
    # 101.00 (idx 1010). Each gets 2.5s.
    assert p.top_nodes(n=10, min_sep_pts=0)[0].seconds == 2.5


def test_bin_indexing_uses_dollar_units() -> None:
    p = VolumeProfile(bin_usd=Decimal("0.10"))
    p.add_bar(_bar("4368.30", "4368.30", "4368.30", "4368.30"))  # single-tick bar
    nodes = p.top_nodes(n=1, min_sep_pts=0)
    assert len(nodes) == 1
    # Bin idx = floor(4368.30 / 0.10) = 43683; center = 4368.30.
    assert nodes[0].price == Decimal("4368.30")


def test_top_nodes_nms_suppresses_close_peaks() -> None:
    """Peaks within min_sep_pts of a higher peak are skipped."""
    p = VolumeProfile(bin_usd=Decimal("0.10"))
    # Build a profile where two adjacent bins both have volume.
    for _ in range(5):
        p.add_bar(_bar("4368.00", "4368.00", "4368.00", "4368.00"))
    for _ in range(5):
        p.add_bar(_bar("4368.20", "4368.20", "4368.20", "4368.20"))  # adjacent bin
    # min_sep_pts=50 = $0.50 = 5 bins apart
    nodes = p.top_nodes(n=5, min_sep_pts=50)
    # The two are only 2 bins apart → only the higher one survives.
    assert len(nodes) == 1


def test_top_nodes_returns_descending_by_volume() -> None:
    p = VolumeProfile(bin_usd=Decimal("0.10"))
    # Three well-separated bins with different volumes.
    for _ in range(2):
        p.add_bar(_bar("4368.00", "4368.00", "4368.00", "4368.00"))
    for _ in range(5):
        p.add_bar(_bar("4370.00", "4370.00", "4370.00", "4370.00"))
    for _ in range(3):
        p.add_bar(_bar("4366.00", "4366.00", "4366.00", "4366.00"))
    # min_sep_pts=150 → $1.50 → 15 bins. The bins are 20 apart, so all survive.
    nodes = p.top_nodes(n=3, min_sep_pts=150)
    assert len(nodes) == 3
    # Highest volume first (4370: 5 bars), then 4366 (3 bars), then 4368 (2 bars).
    assert nodes[0].price == Decimal("4370.00")
    assert nodes[1].price == Decimal("4366.00")
    assert nodes[2].price == Decimal("4368.00")


def test_window_eviction() -> None:
    """Adding more than window_bars evicts old contributions."""
    p = VolumeProfile(bin_usd=Decimal("0.10"), window_bars=3)
    # First 3 bars at price 100
    for _ in range(3):
        p.add_bar(_bar("100.00", "100.00", "100.00", "100.00"))
    assert p.bar_count == 3
    assert p.total_seconds > 0
    # Now add a bar at a different price — should still see only 3 bars.
    p.add_bar(_bar("200.00", "200.00", "200.00", "200.00"))
    assert p.bar_count == 3


def test_single_tick_bar_assigns_full_duration() -> None:
    """A bar with tick_count=1 puts the full duration in the close's bin."""
    p = VolumeProfile(bin_usd=Decimal("0.10"))
    p.add_bar(_bar("4368.00", "4368.00", "4368.00", "4368.00",
                   tick_count=1, duration_s=5.0))
    nodes = p.top_nodes(n=1, min_sep_pts=0)
    assert len(nodes) == 1
    assert nodes[0].seconds == 5.0


def test_median_seconds_handles_empty() -> None:
    p = VolumeProfile()
    assert p.median_seconds() == 0.0


def test_median_seconds_computed_over_nonzero_bins() -> None:
    p = VolumeProfile(bin_usd=Decimal("0.10"))
    # Both bars have O=H=L=C, so each bar's 10s all goes into ONE bin.
    # Bin 43680 (price 4368.00) gets 2 bars × 10s = 20s.
    # Bin 43681 (price 4368.10) gets 1 bar × 10s = 10s.
    p.add_bar(_bar("4368.00", "4368.00", "4368.00", "4368.00"))
    p.add_bar(_bar("4368.00", "4368.00", "4368.00", "4368.00"))
    p.add_bar(_bar("4368.10", "4368.10", "4368.10", "4368.10"))
    # Two non-zero bins: 10.0 and 20.0 → median = (10 + 20) / 2 = 15.0
    assert p.median_seconds() == 15.0


def test_filter_by_min_seconds() -> None:
    p = VolumeProfile(bin_usd=Decimal("0.10"))
    p.add_bar(_bar("4368.00", "4368.00", "4368.00", "4368.00"))  # bin A: 10s (all OHLC same)
    p.add_bar(_bar("4370.00", "4370.00", "4370.00", "4370.00"))  # bin B: 10s
    assert p.total_seconds == 20.0
    p.filter_by_min_seconds(15.0)
    # Both bins have 10s (< 15) → both dropped.
    assert p.total_seconds == 0.0
    # And one above-threshold case:
    p2 = VolumeProfile(bin_usd=Decimal("0.10"))
    p2.add_bar(_bar("4368.00", "4368.00", "4368.00", "4368.00"))
    p2.add_bar(_bar("4368.00", "4368.00", "4368.00", "4368.00"))  # cumulative 20s in this bin
    p2.filter_by_min_seconds(15.0)
    assert p2.total_seconds == 20.0


def test_invalid_config_rejected() -> None:
    import pytest
    with pytest.raises(ValueError, match="bin_usd must be positive"):
        VolumeProfile(bin_usd=Decimal("0"))
    with pytest.raises(ValueError, match="window_bars must be positive"):
        VolumeProfile(window_bars=0)


def test_top_nodes_invalid_n() -> None:
    p = VolumeProfile()
    import pytest
    with pytest.raises(ValueError, match="min_sep_pts must be non-negative"):
        p.top_nodes(n=6, min_sep_pts=-1)


def test_top_nodes_zero_n_returns_empty() -> None:
    p = VolumeProfile()
    p.add_bar(_bar("100.00", "100.00", "100.00", "100.00"))
    assert p.top_nodes(n=0, min_sep_pts=50) == []