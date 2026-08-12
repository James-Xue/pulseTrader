"""Public surface for the Gate.io TradFi REST client."""
from xau.gate.auth import auth_headers, sign
from xau.gate.client import GateClient, GateError, RetryPolicy

__all__ = [
    "GateClient",
    "GateError",
    "RetryPolicy",
    "auth_headers",
    "sign",
]