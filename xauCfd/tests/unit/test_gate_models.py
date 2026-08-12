"""Unit tests for the Gate.io dataclasses: products, account, positions, orders.

These are pure-model tests — no HTTP — verifying that the dataclass
factories tolerate the actual payload shapes we observed in the probe.
"""
from __future__ import annotations

from xau.gate.account import AccountAssets, Mt5Account
from xau.gate.orders import Order, list_open_pendings
from xau.gate.positions import Position, SIDE_BUY, SIDE_SELL
from xau.gate.products import TradFiProduct, get_symbol, list_symbols


# ---------- products ----------

def test_product_from_xauusd_payload() -> None:
    raw = {
        "symbol": "XAUUSD",
        "symbol_desc": "Gold",
        "category_id": 1,
        "trade_mode": 4,
        "status": "open",
        "price_precision": 2,
        "settlement_currency": "USD",
        "leverages": [20, 50, 100, 200, 500],
        "next_open_time": 0,
        # Extra fields the dataclass ignores.
        "some_other_field": "ignored",
    }
    p = TradFiProduct.from_payload(raw)
    assert p.symbol == "XAUUSD"
    assert p.description == "Gold"
    assert p.leverages == (20, 50, 100, 200, 500)
    assert p.settlement_currency == "USD"


def test_product_handles_missing_leverages() -> None:
    p = TradFiProduct.from_payload({"symbol": "X", "leverages": None})
    assert p.leverages == ()


def test_list_symbols_filters_to_match() -> None:
    """Smoke test that get_symbol returns the right product."""
    class _StubClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            return {
                "list": [
                    {"symbol": "XAUUSD", "leverages": [100, 500], "settlement_currency": "USD"},
                    {"symbol": "XAGUSD", "leverages": [50, 100], "settlement_currency": "USD"},
                ],
            }
    p = get_symbol(_StubClient(), "XAUUSD")  # type: ignore[arg-type]
    assert p is not None and p.symbol == "XAUUSD"
    assert get_symbol(_StubClient(), "UNKNOWN") is None  # type: ignore[arg-type]
    assert list_symbols(_StubClient())[0].symbol in {"XAUUSD", "XAGUSD"}  # type: ignore[arg-type]


# ---------- account ----------

def test_mt5_account_from_observed_payload() -> None:
    raw = {
        "is_register": True, "mt5_uid": 5053118, "leverage": 1,
        "server": "", "stop_out_level": "50.000000", "status": 3,
    }
    a = Mt5Account.from_payload(raw)
    assert a.is_register is True
    assert a.mt5_uid == 5053118
    assert a.leverage == 1
    assert a.stop_out_level == "50.000000"
    assert a.status == 3


def test_account_assets_keeps_decimals_as_strings() -> None:
    """Strings preserve precision — never convert to float here."""
    raw = {
        "equity": "60.20", "balance": "60.20",
        "margin": "0.00", "margin_free": "60.20", "outable": "60.20",
        "unrealized_pnl": "0.00", "storage": "0.00",
        "margin_level": "0.00", "trial_used_amount": "0.00",
        "mt5_uid": "5053118",
    }
    a = AccountAssets.from_payload(raw)
    assert a.equity == "60.20"
    assert a.balance == "60.20"
    assert a.margin_level == "0.00"
    assert a.mt5_uid == "5053118"


# ---------- positions ----------

def test_position_from_payload_handles_string_decimals() -> None:
    raw = {
        "position_id": 100, "symbol": "XAUUSD", "position_dir": "Long",
        "volume": "0.04", "price_open": "3400.50", "leverage": "500",
        "unrealized_pnl": "-2.10", "margin": "27.20", "time_create": 1754899200,
    }
    p = Position.from_payload(raw)
    assert p.position_id == 100
    assert p.side == SIDE_BUY
    assert p.volume == "0.04"
    assert p.price == "3400.50"
    assert p.profit == "-2.10"


def test_position_from_payload_maps_position_dir_to_side() -> None:
    """Long → 2 (BUY), Short → 1 (SELL) per official CFD schema."""
    long = Position.from_payload({"position_id": 1, "position_dir": "Long", "symbol": "XAUUSD"})
    short = Position.from_payload({"position_id": 2, "position_dir": "Short", "symbol": "XAUUSD"})
    assert long.side == SIDE_BUY
    assert short.side == SIDE_SELL


def test_position_from_payload_falls_back_to_numeric_side() -> None:
    """Older payloads may carry numeric `side` — keep that path working."""
    p = Position.from_payload({"position_id": 3, "side": 1, "symbol": "XAUUSD"})
    assert p.side == SIDE_SELL


def test_side_constants() -> None:
    # Per official CFD schema (gate.com/docs/developers/apiv4/en/cfd):
    # side=1=sell, side=2=buy. Reversed from typical convention.
    assert SIDE_BUY == 2
    assert SIDE_SELL == 1


# ---------- orders ----------

def test_order_from_observed_payload() -> None:
    raw = {
        "order_id": 5050050, "symbol": "XAUUSD", "base_symbol": "XAUUSD",
        "leverage": "500", "symbol_desc": "Gold", "settlement_currency": "USD",
        "exchange_rate": "1", "price_type": "trigger",
        "state": 1, "state_desc": "", "finished": 0,
        "side": 1, "volume": "0.03", "price": "4367.50",
        "price_tp": "0.00", "price_sl": "0.00",
        "time_setup": 1786431424,
    }
    o = Order.from_payload(raw)
    assert o.order_id == 5050050
    assert o.price_type == "trigger"
    assert o.finished == 0
    assert o.state == 1
    assert o.price == "4367.50"


def test_list_open_pendings_filters_correctly() -> None:
    """list_open_pendings must drop finished orders and filter by symbol."""
    class _StubClient:
        def __init__(self, payload):
            self._payload = payload
        def request(self, method, path, *, params=None, body=None, signed=True):
            return self._payload
    payload = {"list": [
        {"order_id": 1, "symbol": "XAUUSD", "side": 1, "volume": "0.01",
         "price": "3400", "leverage": "500", "price_type": "trigger",
         "state": 1, "finished": 0, "price_tp": "0", "price_sl": "0",
         "time_setup": 0},
        {"order_id": 2, "symbol": "XAUUSD", "side": 1, "volume": "0.01",
         "price": "3401", "leverage": "500", "price_type": "trigger",
         "state": 1, "finished": 1, "price_tp": "0", "price_sl": "0",
         "time_setup": 0},  # finished — should be filtered out
        {"order_id": 3, "symbol": "XAGUSD", "side": 1, "volume": "0.01",
         "price": "30", "leverage": "500", "price_type": "trigger",
         "state": 1, "finished": 0, "price_tp": "0", "price_sl": "0",
         "time_setup": 0},  # wrong symbol — filtered when symbol=XAUUSD
    ]}
    # No symbol filter — keeps state=1+finished=0 across symbols (orders 1 + 3).
    pendings = list_open_pendings(_StubClient(payload))  # type: ignore[arg-type]
    assert {o.order_id for o in pendings} == {1, 3}

    # symbol=XAUUSD — drops order 3 (XAGUSD) but keeps 1 and 2 (2 is finished but
    # the symbol filter applies first in our impl... wait, the symbol filter
    # applies AFTER the state filter. Let me re-check.) Re-run with fresh stub.
    pendings = list_open_pendings(_StubClient(payload), symbol="XAUUSD")  # type: ignore[arg-type]
    # state=1 AND finished=0 filters out order 2, then symbol=XAUUSD keeps order 1.
    assert {o.order_id for o in pendings} == {1}

    pendings = list_open_pendings(_StubClient(payload), symbol="BTCUSD")  # type: ignore[arg-type]
    assert pendings == []


def test_list_orders_handles_null_list() -> None:
    """Server returns {list: null} when no orders — must produce []."""
    class _StubClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            return {"list": None}
    from xau.gate.orders import list_orders
    assert list_orders(_StubClient()) == []  # type: ignore[arg-type]


# ---------- position PUT/POST ----------

def test_close_position_uses_full_close_by_default() -> None:
    """close_position() default = full close, no close_volume field."""
    from xau.gate.positions import close_position, CLOSE_TYPE_FULL
    captured: dict = {}

    class _StubClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            captured["method"] = method
            captured["path"] = path
            captured["body"] = body
            return {"log_id": "123"}

    resp = close_position(_StubClient(), 5066276)  # type: ignore[arg-type]
    assert captured["method"] == "POST"
    assert captured["path"] == "/tradfi/positions/5066276/close"
    assert captured["body"] == {"close_type": CLOSE_TYPE_FULL}
    assert resp == {"log_id": "123"}


def test_close_position_partial_requires_close_volume() -> None:
    """close_type=1 must carry close_volume."""
    from xau.gate.positions import close_position, CLOSE_TYPE_PARTIAL
    captured: dict = {}

    class _StubClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            captured["body"] = body
            return {}

    close_position(_StubClient(), 1,  # type: ignore[arg-type]
                    close_type=CLOSE_TYPE_PARTIAL, close_volume="0.01")
    assert captured["body"] == {"close_type": CLOSE_TYPE_PARTIAL, "close_volume": "0.01"}


def test_close_position_partial_without_volume_raises() -> None:
    from xau.gate.positions import close_position, CLOSE_TYPE_PARTIAL
    with __import__("pytest").raises(ValueError):
        close_position(None, 1,  # type: ignore[arg-type]
                        close_type=CLOSE_TYPE_PARTIAL)


def test_update_position_sl_tp_sends_both_fields() -> None:
    from xau.gate.positions import update_position_sl_tp
    captured: dict = {}

    class _StubClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            captured["method"] = method
            captured["path"] = path
            captured["body"] = body
            return {}

    update_position_sl_tp(
        _StubClient(), 5066276,  # type: ignore[arg-type]
        price_sl="4389.87", price_tp="4391.07",
    )
    assert captured["method"] == "PUT"
    assert captured["path"] == "/tradfi/positions/5066276"
    assert captured["body"] == {"price_sl": "4389.87", "price_tp": "4391.07"}


def test_update_position_sl_tp_omits_none_fields() -> None:
    """If a field is None, do not include it in the body — broker treats
    absent fields as 'keep existing'."""
    from xau.gate.positions import update_position_sl_tp
    captured: dict = {}

    class _StubClient:
        def request(self, method, path, *, params=None, body=None, signed=True):
            captured["body"] = body
            return {}

    update_position_sl_tp(
        _StubClient(), 1,  # type: ignore[arg-type]
        price_sl="4389.87", price_tp=None,
    )
    assert captured["body"] == {"price_sl": "4389.87"}

    update_position_sl_tp(
        _StubClient(), 1,  # type: ignore[arg-type]
        price_sl=None, price_tp="4391.07",
    )
    assert captured["body"] == {"price_tp": "4391.07"}