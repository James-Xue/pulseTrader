"""Split volume profile nodes into support (below mid) and resistance (above).

The reversion strategy places BUY_LIMITs just BELOW support levels (so the
limit only fills on a dip into support and we get a reversion bounce) and
SELL_LIMITs just ABOVE resistance levels. Levels too close to `mid` (within
`min_distance_usd`) get filled immediately and are skipped; levels too far
(in a different regime) get filtered by `max_distance_usd`.

Distance filter is in DOLLARS (user-spec 2026-08-11); converted to points
(×100) inside this module.
"""
from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal
from enum import Enum

from xau.market_data.volume_profile import Node


class LevelKind(str, Enum):
    SUPPORT = "SUPPORT"
    RESISTANCE = "RESISTANCE"


@dataclass(frozen=True, slots=True)
class Level:
    """One S/R level extracted from the volume profile."""
    price: Decimal       # bin center
    kind: LevelKind
    seconds: float       # time-at-price from the profile
    distance_pts: float   # signed: negative for support, positive for resistance


def extract_levels(
    nodes: list[Node],
    mid: Decimal,
    *,
    min_distance_usd: float = 15.0,  # skip levels within $15 of mid (was $0.15)
    max_distance_usd: float = 80.0,  # skip levels beyond $80 of mid
) -> list[Level]:
    """Split nodes into SUPPORT (below mid) and RESISTANCE (above).

    Distance bounds are in DOLLARS; converted to points (×100) internally.
    Levels closer than `min_distance_usd` are dropped (instant-fill limits).
    Levels farther than `max_distance_usd` are dropped (different regime,
    likely stale). The result is sorted by `distance_pts` ascending — closest
    level to mid first, regardless of side. Callers can re-sort by side if
    needed.
    """
    min_distance_pts = min_distance_usd * 100
    max_distance_pts = max_distance_usd * 100
    levels: list[Level] = []
    for n in nodes:
        diff = n.price - mid
        diff_pts = float(diff) * 100  # price units → points (1 pt = $0.01)
        abs_pts = abs(diff_pts)
        if abs_pts < min_distance_pts:
            continue
        if abs_pts > max_distance_pts:
            continue
        kind = LevelKind.SUPPORT if diff < 0 else LevelKind.RESISTANCE
        levels.append(Level(
            price=n.price,
            kind=kind,
            seconds=n.seconds,
            distance_pts=diff_pts,
        ))
    levels.sort(key=lambda lv: abs(lv.distance_pts))
    return levels