"""Structured logging via structlog.

Two sinks:
  * Console (stderr) — colorized `key=value` rendering, default for dev
  * File (optional) — line-delimited JSON, default for production

Usage:
    from xau.infra.logging import setup_logging, get_logger
    setup_logging(level="INFO", json_path=Path("logs/daemon.jsonl"))
    log = get_logger("daemon")
    log.info("placed_order", order_id=123, side="BUY", price=4367.50)

`get_logger(name)` returns a structlog logger bound to `name` as `module`.
Callers can also use `structlog.get_logger(name)` directly after setup.
"""
from __future__ import annotations

import logging
import sys
from pathlib import Path
from typing import Any

import structlog


def setup_logging(
    *,
    level: str = "INFO",
    json_path: Path | None = None,
    also_console: bool = True,
) -> None:
    """Configure structlog + stdlib logging.

    `level` — root level for both sinks. One of DEBUG/INFO/WARNING/ERROR.
    `json_path` — if set, append every record as JSON to this file.
    `also_console` — whether to mirror to stderr (default True).
    """
    log_level = getattr(logging, level.upper(), logging.INFO)

    timestamper = structlog.processors.TimeStamper(fmt="iso", utc=True)

    shared_processors: list[Any] = [
        structlog.contextvars.merge_contextvars,
        structlog.stdlib.add_log_level,
        structlog.stdlib.add_logger_name,
        timestamper,
        structlog.processors.StackInfoRenderer(),
        structlog.processors.format_exc_info,
    ]

    # stdlib bridge — captures anything written via `logging` too (httpx, urllib, etc.)
    root = logging.getLogger()
    # Close any pre-existing handlers from a prior setup call so we don't leak
    # file descriptors when tests (or hot-reloads) call setup_logging repeatedly.
    for h in list(root.handlers):
        try:
            h.close()
        except Exception:
            pass
    root.handlers = []
    root.setLevel(log_level)

    handler = logging.StreamHandler(sys.stderr)
    handler.setLevel(log_level)
    root.addHandler(handler)

    if json_path is not None:
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_handler = logging.FileHandler(json_path, encoding="utf-8")
        json_handler.setLevel(log_level)
        root.addHandler(json_handler)

    structlog.configure(
        processors=[
            *shared_processors,
            structlog.stdlib.ProcessorFormatter.wrap_for_formatter,
        ],
        wrapper_class=structlog.make_filtering_bound_logger(log_level),
        logger_factory=structlog.stdlib.LoggerFactory(),
        cache_logger_on_first_use=True,
    )

    console_formatter = structlog.stdlib.ProcessorFormatter(
        foreign_pre_chain=shared_processors,
        processors=[
            structlog.stdlib.ProcessorFormatter.remove_processors_meta,
            structlog.dev.ConsoleRenderer(colors=sys.stderr.isatty()) if also_console else _noop_renderer,
        ],
    )
    handler.setFormatter(console_formatter)

    if json_path is not None:
        json_formatter = structlog.stdlib.ProcessorFormatter(
            foreign_pre_chain=shared_processors,
            processors=[
                structlog.stdlib.ProcessorFormatter.remove_processors_meta,
                structlog.processors.JSONRenderer(),
            ],
        )
        json_handler.setFormatter(json_formatter)


def _noop_renderer(_: Any, __: str) -> str:  # pragma: no cover — used when console disabled
    return ""


def get_logger(name: str) -> structlog.stdlib.BoundLogger:
    """Get a structlog logger. Returns a BoundLogger with `module=name`."""
    return structlog.get_logger(name)