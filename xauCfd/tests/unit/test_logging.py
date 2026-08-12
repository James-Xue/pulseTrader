"""Tests for setup_logging + get_logger."""
from __future__ import annotations

import io
import logging
import sys
from pathlib import Path

import structlog

from xau.infra.logging import get_logger, setup_logging


def test_setup_logging_does_not_raise(tmp_path: Path) -> None:
    setup_logging(level="INFO", json_path=tmp_path / "x.jsonl")
    log = get_logger("test")
    log.info("hello", x=1)
    # File should have a JSON line.
    body = (tmp_path / "x.jsonl").read_text()
    assert "hello" in body
    assert "x" in body


def test_json_lines_contain_kv(tmp_path: Path) -> None:
    setup_logging(level="INFO", json_path=tmp_path / "y.jsonl")
    log = get_logger("my.module")
    log.info("event_name", price=4368.50, side="BUY")
    lines = (tmp_path / "y.jsonl").read_text().strip().splitlines()
    assert len(lines) == 1
    # structlog JSON output includes the event and key/value pairs.
    assert "event_name" in lines[0]
    assert "4368.5" in lines[0] or "4368.50" in lines[0]
    assert "BUY" in lines[0]


def test_log_level_filters(tmp_path: Path) -> None:
    setup_logging(level="WARNING", json_path=tmp_path / "z.jsonl")
    log = get_logger("filter")
    log.info("not_kept")
    log.warning("kept")
    body = (tmp_path / "z.jsonl").read_text()
    assert "not_kept" not in body
    assert "kept" in body


def test_logger_carries_module_name(tmp_path: Path) -> None:
    setup_logging(level="INFO", json_path=tmp_path / "m.jsonl")
    log = get_logger("daemon.cycle")
    log.info("started")
    assert "daemon.cycle" in (tmp_path / "m.jsonl").read_text()


def test_get_logger_returns_structlog_bound_logger(tmp_path: Path) -> None:
    setup_logging(level="INFO", json_path=tmp_path / "b.jsonl")
    log = get_logger("x")
    # BoundLogger has .info/.warning/.error/.debug and bind().
    assert hasattr(log, "info")
    assert hasattr(log, "bind")
    assert callable(log.info)