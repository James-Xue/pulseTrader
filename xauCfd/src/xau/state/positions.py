"""In-memory position state for the daemon.

Tracks open positions across cycles: entry price, entry time, and the SL/TP
levels last pushed to the broker. Used by `apps.daemon._position_management`
to drive auto-close (orphans), SL/TP push (PUT /tradfi/positions/{id}), and
momentum-TP logic (`risk.momentum.MomentumTpPolicy`).

Per-position state is in-memory only — if the daemon restarts, the book
rebuilds from `list_positions`. `entry_time` falls back to the broker's
`Position.time_setup` (the position's open timestamp) when present, else
`clock.now()`. Worst case after restart: the momentum window has already
expired, so the policy simply doesn't fire — the daemon still pushes SL/TP.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from decimal import Decimal
from typing import Protocol


class _ClockLike(Protocol):
    """Subset of xau.infra.clock.Clock we use. Kept loose to avoid a hard import."""

    def now(self) -> datetime: ...


def _ts(dt: datetime) -> float:
    """Convert a tz-aware datetime to unix seconds. Naive → assume UTC."""
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.timestamp()


@dataclass(frozen=True, slots=True)
class PositionRecord:
    """One open position, daemon-side view."""
    position_id: int
    symbol: str
    side: int          # 2=BUY/Long, 1=SELL/Short (per CFD schema)
    entry_price: Decimal
    entry_time: float   # unix seconds
    last_sl: Decimal | None = None  # last value pushed via PUT (None = never)
    last_tp: Decimal | None = None


@dataclass(slots=True)
class PositionBook:
    """Per-position state, keyed by position_id.

    `clock` provides `now()` (tz-aware datetime) used when the broker payload
    omits `time_setup`.
    """
    clock: _ClockLike
    records: dict[int, PositionRecord] = field(default_factory=dict)

    def mark_seen(
        self,
        positions: list,
        *,
        now: datetime | None = None,
    ) -> tuple[list[PositionRecord], list[PositionRecord]]:
        """Sync `records` against the latest broker positions snapshot.

        Returns `(new_records, all_records)`:
          * `new_records` = records first seen THIS call (i.e. fills since last cycle).
            Used by the daemon to push SL/TP immediately on detection.
          * `all_records` = the current book contents, in iteration order.

        For each position seen:
          * already-known → keep entry/entry_time as recorded; do NOT refresh.
          * new           → create record; entry_time =
              `Position.time_setup` if > 0, else `clock.now()`.

        Records for positions no longer in `positions` are dropped (they were
        closed by something — broker, manual, or this daemon on a prior cycle).
        """
        now = now or self.clock.now()
        new_records: list[PositionRecord] = []
        seen_ids: set[int] = set()
        for p in positions:
            pid = int(getattr(p, "position_id", 0))
            if pid <= 0:
                continue
            seen_ids.add(pid)
            existing = self.records.get(pid)
            if existing is not None:
                continue
            entry_price = Decimal(str(getattr(p, "price", "0")))
            entry_time = float(getattr(p, "time_setup", 0) or 0)
            if entry_time <= 0:
                entry_time = _ts(now)
            rec = PositionRecord(
                position_id=pid,
                symbol=str(getattr(p, "symbol", "")),
                side=int(getattr(p, "side", 0)),
                entry_price=entry_price,
                entry_time=entry_time,
            )
            self.records[pid] = rec
            new_records.append(rec)

        # Forget positions no longer on the broker.
        for pid in list(self.records):
            if pid not in seen_ids:
                self.records.pop(pid, None)

        return new_records, list(self.records.values())

    def forget(self, position_id: int) -> None:
        """Drop a record (e.g. after a successful close)."""
        self.records.pop(int(position_id), None)

    def update_sl_tp(self, position_id: int, *, sl: Decimal, tp: Decimal) -> None:
        """Memo: record the SL/TP just pushed. No-op if absent."""
        rec = self.records.get(int(position_id))
        if rec is None:
            return
        self.records[int(position_id)] = PositionRecord(
            position_id=rec.position_id,
            symbol=rec.symbol,
            side=rec.side,
            entry_price=rec.entry_price,
            entry_time=rec.entry_time,
            last_sl=sl,
            last_tp=tp,
        )

    def should_update_sl_tp(
        self, record: PositionRecord, *, sl: Decimal, tp: Decimal,
    ) -> bool:
        """Return True if (sl, tp) differ from last-pushed memo."""
        return record.last_sl != sl or record.last_tp != tp