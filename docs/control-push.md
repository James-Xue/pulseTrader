# Control-socket market-data push

An external strategy runtime can subscribe to closed klines over the control
socket instead of polling `get_market`. The push shares the existing
connection, TLS and token — there is no second port and no second auth path.

## Why

Polling `get_market` cannot be made gap-free: the endpoint returns a snapshot
of an in-memory ring, so a client that polls slower than the bar interval
misses bars, and a client that polls faster re-reads the same bar. The push
channel delivers exactly one event per finished bar.

## Methods

Both are **control-socket-only**. MCP is request/response and has no
server-push channel, so they are not exposed as MCP tools.

### `subscribe_market`

```json
{"jsonrpc":"2.0","id":1,"method":"subscribe_market","params":{
  "channel":"kline",
  "symbols":["BTC_USDT"],
  "market_type":"futures"
}}
```

| Field | Required | Meaning |
|---|---|---|
| `channel` | no | Only `kline` today (default `kline`). |
| `symbols` | no | Array of symbols. Omitted/empty = every symbol of the market. |
| `market_type` | no | `spot` / `futures` / `cfd`. Omitted/empty = any market. |

Reply:

```json
{"jsonrpc":"2.0","id":1,"result":{
  "subscribed":true,"channel":"kline","market_type":"futures",
  "symbols":["BTC_USDT"],
  "note":"notifications are pushed only for CLOSED klines"
}}
```

A subscription replaces any previous one on the same connection.

### `unsubscribe_market`

```json
{"jsonrpc":"2.0","id":2,"method":"unsubscribe_market"}
```

Idempotent; replies `{"subscribed":false}` even when nothing was subscribed.

### Notifications

The server initiates these; they carry **no `id`**:

```json
{"jsonrpc":"2.0","method":"market.kline","params":{
  "symbol":"BTC_USDT","market_type":"futures",
  "open_time":1794620000000,"close_time":1794620060000,
  "open":50000.0,"high":50010.0,"low":49990.0,"close":50005.0,
  "volume":1.25,"closed":true
}}
```

`open_time`/`close_time` are epoch milliseconds. `closed` is always `true` on
this channel — a forming bar is never published.

## Semantics and guarantees

- **One event per bar.** Gate re-pushes a bar every couple of seconds while it
  forms; the feed upserts into the ring by `open_time` and emits only when the
  bar is final (`w:true` on futures, `close_time` passed on spot).
- **Closed bars only.** Strategies never see a half-formed candle. This is the
  B6 fix: the pre-2026-11-14 feed marked every push `closed=true`.
- **Interval.** 1 minute. Coarser intervals are not buffered by the engine.
- **Ordering.** Per symbol, ascending `open_time`. No cross-symbol ordering.
- **Delivery.** At-most-once. There is no replay and no acknowledgement: after
  a reconnect, backfill with `get_market` (`klines` parameter) or read
  `kline_bars` from SQLite.

## Backpressure

Each subscriber owns a bounded queue of 1024 notifications. When a client
falls behind, the **oldest** notification is dropped so the freshest bar
survives. `publish()` only enqueues, under the subscriber's own mutex, so a
stalled or dead client can never block the market-data hot path.

A client that is gone is detected on the next write; the session is marked
stopped and removed from the subscriber list.

## Example (Python)

```python
import json, socket

sock = socket.create_connection(("127.0.0.1", 8081))
f = sock.makefile("rwb")

def call(obj):
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    return json.loads(f.readline())

print(call({"jsonrpc": "2.0", "id": 1, "method": "subscribe_market",
            "params": {"channel": "kline", "symbols": ["BTC_USDT"],
                       "market_type": "futures"}}))

while True:
    msg = json.loads(f.readline())
    if msg.get("method") == "market.kline":
        bar = msg["params"]
        print(bar["symbol"], bar["open_time"], bar["close"])
```

If `[control] token` is set, send the auth line first and wait for its reply:

```python
call({"jsonrpc": "2.0", "id": 0, "method": "auth",
      "params": {"token": "<token>"}})
```

## Limitations

- Only the `kline` channel exists today. Order/fill notifications are not
  pushed; poll `get_orders` (fills surface within ~3 s via the throttled
  tracker reconcile).
- The subscriber is tied to its connection: closing the socket ends the
  subscription.
