"""HTTP client for the Gate.io TradFi REST API.

Wraps `httpx.Client` with:
  * auto-signing every request via `gate.auth.auth_headers`
  * bounded retries on 5xx and connection errors (NOT 4xx — auth errors
    and validation errors must surface immediately)
  * exponential backoff capped at `max_backoff_s`
  * uniform JSON envelope handling: server wraps every payload in
    `{"label": "...", "data": ..., "timestamp": <ms>}`. We unwrap `data`
    and surface `label` / `message` from non-2xx bodies as `GateError`.
  * explicit timeout per request (`req_timeout_s`)

The client owns its httpx connection; close it via context manager or
`.close()`. The client is **not thread-safe** for the same `_client`
instance — make one per thread or use `httpx.Client` directly per worker.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import urlencode

import httpx

from xau.gate.auth import auth_headers

log = logging.getLogger("gate.client")


class GateError(RuntimeError):
    """Raised for non-2xx responses. Carries the server's message verbatim."""

    def __init__(self, status: int, message: str, label: str = "", payload: Any = None) -> None:
        super().__init__(f"gate {status} {label}: {message}")
        self.status = status
        self.message = message
        self.label = label
        self.payload = payload


@dataclass(slots=True)
class RetryPolicy:
    """Retries on 5xx and transport errors. Never on 4xx."""
    max_retries: int = 3
    initial_backoff_s: float = 0.2
    max_backoff_s: float = 2.0

    def backoff(self, attempt: int) -> float:
        # exponential: 0.2, 0.4, 0.8, 1.6 (capped at max_backoff_s)
        return min(self.initial_backoff_s * (2 ** attempt), self.max_backoff_s)

    def should_retry(self, exc_or_status: httpx.HTTPError | int) -> bool:
        if isinstance(exc_or_status, int):
            return 500 <= exc_or_status < 600
        # httpx.HTTPError covers ConnectError, ReadTimeout, etc.
        # DO NOT retry on HTTPStatusError — that's a real response.
        return not isinstance(exc_or_status, httpx.HTTPStatusError)


class GateClient:
    """Synchronous REST client. Use as a context manager."""

    def __init__(
        self,
        api_key: str,
        api_secret: str,
        base_url: str = "https://api.gateio.ws/api/v4",
        req_timeout_s: float = 5.0,
        retry: RetryPolicy | None = None,
        http_client: httpx.Client | None = None,
    ) -> None:
        # Never log api_secret — only the prefix is acceptable in debug.
        self._key = api_key
        self._secret = api_secret
        self._base = base_url.rstrip("/")
        self._timeout = req_timeout_s
        self._retry = retry or RetryPolicy()
        self._http = http_client or httpx.Client(timeout=req_timeout_s)
        self._owns_http = http_client is None

    @property
    def base_url(self) -> str:
        return self._base

    def close(self) -> None:
        if self._owns_http:
            self._http.close()

    def __enter__(self) -> GateClient:
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def request(
        self,
        method: str,
        path: str,
        *,
        params: dict[str, Any] | None = None,
        body: dict[str, Any] | None = None,
        signed: bool = True,
    ) -> Any:
        """Send an HTTP request, signing it if `signed=True`.

        Returns the unwrapped `data` field of the response envelope. Raises
        `GateError` for non-2xx responses (after retry exhaustion if 5xx).
        """
        url_path = path if path.startswith("/") else f"/{path}"
        full_url = f"{self._base}{url_path}"

        # Encode query once for both signing and request.
        encoded_query = urlencode(params, doseq=True) if params else ""

        # Body serialization (the server expects JSON on POST). We must
        # sign the EXACT bytes that go on the wire, so we serialize here
        # and pass raw bytes (NOT httpx's `json=` parameter, which would
        # re-serialize with different whitespace/ordering).
        body_str = ""
        body_bytes: bytes | None = None
        if body is not None:
            import json as _json
            body_str = _json.dumps(body, separators=(",", ":"), sort_keys=True)
            body_bytes = body_str.encode("utf-8")

        # Signed path mirrors `urlparse(full_url).path` from the official SDK.
        # That gives us `/api/v4/tradfi/...` — i.e. the path INCLUDING the
        # `/api/v4` prefix that `self._base` ends with. We strip only the
        # scheme + host.
        from urllib.parse import urlparse
        sign_path = urlparse(full_url).path  # `/api/v4/tradfi/...`

        headers = {"Content-Type": "application/json"}
        if signed:
            headers.update(auth_headers(
                self._key, self._secret, method, sign_path,
                query=encoded_query, body=body_str,
            ))

        last_exc: Exception | None = None
        for attempt in range(self._retry.max_retries + 1):
            try:
                resp = self._http.request(
                    method,
                    full_url,
                    params=encoded_query or None,
                    content=body_bytes,
                    headers=headers,
                )
            except httpx.HTTPError as exc:
                last_exc = exc
                if attempt < self._retry.max_retries and self._retry.should_retry(exc):
                    delay = self._retry.backoff(attempt)
                    log.warning("gate transport error (attempt %d): %s — retry in %.2fs",
                                attempt + 1, exc, delay)
                    import time as _time
                    _time.sleep(delay)
                    continue
                raise

            if 200 <= resp.status_code < 300:
                try:
                    envelope = resp.json()
                except Exception:
                    # Non-JSON 2xx (rare). Return raw text.
                    return resp.text
                # Unwrap data envelope if present.
                if isinstance(envelope, dict) and "data" in envelope:
                    return envelope["data"]
                return envelope

            # Non-2xx — read body and decide retry.
            try:
                payload = resp.json()
            except Exception:
                payload = {"message": resp.text[:200], "label": ""}

            err = GateError(
                status=resp.status_code,
                message=str(payload.get("message", "")),
                label=str(payload.get("label", "")),
                payload=payload,
            )
            if self._retry.should_retry(resp.status_code) and attempt < self._retry.max_retries:
                last_exc = err
                delay = self._retry.backoff(attempt)
                log.warning("gate %d (attempt %d) %s — retry in %.2fs",
                            resp.status_code, attempt + 1, err.message, delay)
                import time as _time
                _time.sleep(delay)
                continue
            raise err

        # Should be unreachable.
        if last_exc:
            raise last_exc
        raise GateError(status=0, message="unreachable", label="")