"""Tests for config loader: TOML parsing, from_env substitution, sections."""
from __future__ import annotations

import os
from pathlib import Path

import pytest

from xau.infra.config import load_config, section


def test_loads_basic_toml(tmp_path: Path) -> None:
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text(
        "[broker]\n"
        "symbol = 'XAUUSD'\n"
        "leverage = 500\n"
        "\n"
        "[risk]\n"
        "risk_pct = 0.0025\n"
        "max_lot = 0.10\n",
        encoding="utf-8",
    )
    cfg = load_config(cfg_path)
    assert section(cfg, "broker") == {"symbol": "XAUUSD", "leverage": 500}
    assert section(cfg, "risk") == {"risk_pct": 0.0025, "max_lot": 0.10}


def test_missing_file_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        load_config(tmp_path / "nope.toml")


def test_from_env_string_resolved(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("XAU_TEST_KEY", "abc123")
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text(
        "[broker]\napi_key = 'from_env:XAU_TEST_KEY'\n", encoding="utf-8"
    )
    cfg = load_config(cfg_path)
    assert section(cfg, "broker")["api_key"] == "abc123"


def test_from_env_string_missing_required_raises(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.delenv("XAU_MISSING", raising=False)
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text(
        "[broker]\napi_key = 'from_env:XAU_MISSING'\n", encoding="utf-8"
    )
    with pytest.raises(KeyError, match="XAU_MISSING"):
        load_config(cfg_path)


def test_from_env_with_default(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.delenv("XAU_OPTIONAL", raising=False)
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text(
        "[broker]\napi_key = 'from_env:XAU_OPTIONAL:-fallback_value'\n", encoding="utf-8"
    )
    cfg = load_config(cfg_path)
    assert section(cfg, "broker")["api_key"] == "fallback_value"


def test_from_env_with_default_overridden_by_env(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("XAU_OPTIONAL", "real_value")
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text(
        "[broker]\napi_key = 'from_env:XAU_OPTIONAL:-fallback'\n", encoding="utf-8"
    )
    cfg = load_config(cfg_path)
    assert section(cfg, "broker")["api_key"] == "real_value"


def test_env_substitution_in_list(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("XAU_A", "alpha")
    monkeypatch.setenv("XAU_B", "beta")
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text(
        "tags = ['from_env:XAU_A', 'plain', 'from_env:XAU_B']\n", encoding="utf-8"
    )
    cfg = load_config(cfg_path)
    assert cfg["tags"] == ["alpha", "plain", "beta"]


def test_env_substitution_in_nested_dict(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setenv("XAU_SECRET", "s3cr3t")
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text(
        "[outer.inner]\nsecret = 'from_env:XAU_SECRET'\n", encoding="utf-8"
    )
    cfg = load_config(cfg_path)
    assert cfg["outer"]["inner"]["secret"] == "s3cr3t"


def test_plain_string_passes_through(tmp_path: Path) -> None:
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text("name = 'literal'\n", encoding="utf-8")
    cfg = load_config(cfg_path)
    assert cfg["name"] == "literal"


def test_section_returns_empty_dict_when_missing(tmp_path: Path) -> None:
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text("[broker]\nsymbol = 'XAUUSD'\n", encoding="utf-8")
    cfg = load_config(cfg_path)
    assert section(cfg, "nope") == {}


def test_section_raises_on_non_dict(tmp_path: Path) -> None:
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text("broker = 'not a table'\n", encoding="utf-8")
    cfg = load_config(cfg_path)
    with pytest.raises(TypeError, match="must be a table"):
        section(cfg, "broker")


def test_int_and_float_preserved(tmp_path: Path) -> None:
    cfg_path = tmp_path / "xau.toml"
    cfg_path.write_text("i = 42\nf = 3.14\n", encoding="utf-8")
    cfg = load_config(cfg_path)
    assert cfg["i"] == 42
    assert cfg["f"] == 3.14