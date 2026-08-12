"""Tests for ticker snapshot parsing and the fetch wrapper."""
from __future__ import annotations

from decimal import Decimal

from xau.market_data.ticker import Ticker, fetch_ticker


def test_from_payload_decimal_strings_preserved() -> None:
    raw = {
        "highest_price": "4435.31",
        "lowest_price": "4356.77",
        "price_change": "-0.50",
        "price_change_amount": "-22.00",
        "today_open_price": "4391.66",
        "last_today_close_price": "4390.29",
        "last_price": "4368.29",
        "bid_price": "4368.31",
        "ask_price": "4368.41",
        "close_time": 1754889600,
    }
    t = Ticker.from_payload("XAUUSD", raw)
    assert t.symbol == "XAUUSD"
    assert t.last_price == "4368.29"
    assert t.bid_price == "4368.31"
    assert t.ask_price == "4368.41"
    assert t.today_high == "4435.31"
    assert t.today_low == "4356.77"
    assert t.price_change_pct == "-0.50"
    assert t.close_time_s == 1754889600


def test_mid_averages_bid_ask() -> None:
    t = Ticker(
        symbol="XAUUSD",
        last_price="4368.29",
        bid_price="4368.30",
        ask_price="4368.40",
        today_open="0", today_high="0", today_low="0",
        price_change_pct="0", close_time_s=0,
    )
    assert Decimal(t.mid()) == Decimal("4368.35")


def test_mid_handles_widened_spread() -> None:
    t = Ticker(
        symbol="XAUUSD",
        last_price="0", bid_price="4370.00", ask_price="4372.00",
        today_open="0", today_high="0", today_low="0",
        price_change_pct="0", close_time_s=0,
    )
    assert Decimal(t.mid()) == Decimal("4371.00")


def test_from_payload_defaults_missing_fields() -> None:
    """Server sometimes returns sparse data — all fields default to '0'/0."""
    t = Ticker.from_payload("X", {})
    assert t.symbol == "X"
    assert t.last_price == "0"
    assert t.bid_price == "0"
    assert t.close_time_s == 0


def test_fetch_ticker_calls_client_with_correct_path() -> None:
    class FakeClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            assert method == "GET"
            assert path == "/tradfi/symbols/XAUUSD/tickers"
            return {
                "last_price": "4368.29",
                "bid_price": "4368.31",
                "ask_price": "4368.41",
                "highest_price": "4435.31",
                "lowest_price": "4356.77",
                "today_open_price": "4391.66",
                "price_change": "-0.50",
                "close_time": 1754889600,
            }
    t = fetch_ticker(FakeClient())  # type: ignore[arg-type]
    assert isinstance(t, Ticker)
    assert t.last_price == "4368.29"


def test_fetch_ticker_raises_on_non_dict_payload() -> None:
    class FakeClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            return "unexpected string"
    import pytest
    with pytest.raises(ValueError, match="unexpected ticker payload type"):
        fetch_ticker(FakeClient())  # type: ignore[arg-type]


def test_fetch_ticker_custom_symbol() -> None:
    captured = {}
    class FakeClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            captured["path"] = path
            return {"last_price": "0", "bid_price": "0", "ask_price": "0"}
    fetch_ticker(FakeClient(), symbol="XAGUSD")  # type: ignore[arg-type]
    assert captured["path"] == "/tradfi/symbols/XAGUSD/tickers"