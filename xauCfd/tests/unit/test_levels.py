"""Tests for level extraction from volume profile nodes."""
from __future__ import annotations

from decimal import Decimal

from xau.market_data.levels import LevelKind, extract_levels
from xau.market_data.volume_profile import Node


def test_empty_nodes_yields_empty_levels() -> None:
    levels = extract_levels([], Decimal("4368.00"))
    assert levels == []


def test_support_below_mid_resistance_above() -> None:
    nodes = [
        Node(Decimal("4366.50"), 5.0),  # $1.50 below mid
        Node(Decimal("4369.50"), 4.0),  # $1.50 above mid
    ]
    # Use a wider max here to keep both; lower min so $1.50 isn't filtered.
    levels = extract_levels(
        nodes, Decimal("4368.00"),
        min_distance_usd=0.50, max_distance_usd=2.0,
    )
    assert len(levels) == 2
    support = [lv for lv in levels if lv.kind == LevelKind.SUPPORT]
    resist = [lv for lv in levels if lv.kind == LevelKind.RESISTANCE]
    assert len(support) == 1 and support[0].price == Decimal("4366.50")
    assert len(resist) == 1 and resist[0].price == Decimal("4369.50")


def test_level_distance_is_signed() -> None:
    nodes = [Node(Decimal("4367.00"), 1.0), Node(Decimal("4369.00"), 1.0)]
    levels = extract_levels(
        nodes, Decimal("4368.00"),
        min_distance_usd=0.50, max_distance_usd=2.0,
    )
    support = [lv for lv in levels if lv.kind == LevelKind.SUPPORT][0]
    resist = [lv for lv in levels if lv.kind == LevelKind.RESISTANCE][0]
    assert support.distance_pts < 0
    assert resist.distance_pts > 0
    assert support.distance_pts == -100.0  # $1.00 = 100 points, but below mid so negative
    assert resist.distance_pts == 100.0


def test_levels_too_close_to_mid_are_dropped() -> None:
    nodes = [Node(Decimal("4368.10"), 5.0), Node(Decimal("4367.90"), 5.0)]
    # Both are $0.10 from mid; min $0.30 (30 pts) drops them.
    levels = extract_levels(nodes, Decimal("4368.00"), min_distance_usd=0.30)
    assert levels == []


def test_levels_too_far_are_dropped() -> None:
    nodes = [Node(Decimal("4380.00"), 5.0), Node(Decimal("4356.00"), 5.0)]
    # Both $12 / $12 from mid; max $0.80 (80 pts) drops them.
    levels = extract_levels(
        nodes, Decimal("4368.00"),
        min_distance_usd=0.10, max_distance_usd=0.80,
    )
    assert levels == []


def test_levels_sorted_by_distance_ascending() -> None:
    nodes = [
        Node(Decimal("4366.50"), 1.0),  # $1.50 away
        Node(Decimal("4367.00"), 1.0),  # $1.00 away — closer
        Node(Decimal("4367.50"), 1.0),  # $0.50 away — closest
    ]
    levels = extract_levels(
        nodes, Decimal("4368.00"),
        min_distance_usd=0.50, max_distance_usd=2.0,
    )
    assert [lv.price for lv in levels] == [
        Decimal("4367.50"), Decimal("4367.00"), Decimal("4366.50"),
    ]


def test_levels_preserves_seconds() -> None:
    nodes = [Node(Decimal("4367.00"), 7.5)]
    levels = extract_levels(
        nodes, Decimal("4368.00"),
        min_distance_usd=0.50, max_distance_usd=2.0,
    )
    assert levels[0].seconds == 7.5


def test_custom_distance_thresholds() -> None:
    nodes = [
        Node(Decimal("4367.50"), 1.0),  # $0.50
        Node(Decimal("4366.00"), 1.0),  # $2.00
    ]
    levels = extract_levels(nodes, Decimal("4368.00"),
                            min_distance_usd=0.20, max_distance_usd=1.00)
    # Only $0.50 one survives ($2.00 > $1.00 max).
    assert len(levels) == 1
    assert levels[0].price == Decimal("4367.50")


def test_levels_at_exact_min_and_max_are_inclusive() -> None:
    # $0.30 below mid (exactly the min threshold).
    nodes = [Node(Decimal("4367.70"), 1.0)]
    levels = extract_levels(nodes, Decimal("4368.00"),
                            min_distance_usd=0.30, max_distance_usd=0.30)
    assert len(levels) == 1
    assert levels[0].price == Decimal("4367.70")