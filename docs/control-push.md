# Control-socket push channels

External clients can subscribe to server-initiated events over the control
socket instead of polling. Two channels exist:

| Channel | Subscribe with | Notification method | Content |
|---|---|---|---|
| `kline` | `subscribe_market` | `market.kline` | one event per CLOSED 1-minute bar |
| `order` | `subscribe_orders` | `order.update` | order lifecycle events |

Both share the existing connection, TLS and token — there is no second port
and no second auth path. A single connection can hold **one subscription per
channel**, so a runtime can watch bars and its own orders over the same socket.

## Why

Polling `get_market` cannot be made gap-free: the endpoint returns a snapshot
of an in-memory ring, so a client that polls slower than the bar interval
misses bars, and a client that polls faster re-reads the same bar. The push
channel delivers exactly one event per finished bar.

Polling `get_orders` for order state is possible but forces a poll interval
that trades latency against REST load. `order.update` delivers the transition
as it happens, so a strategy runtime can react to a fill immediately.

## Methods

All four are **control-socket-only**. MCP is request/response and has no
server-push channel, so they are not exposed as MCP tools — an MCP client
still polls `get_market` / `get_orders`.

### `subscribe_market` — channel `kline`

```json
{"jsonrpc":"2.0","id":1,"method":"subscribe_market","params":{
  "channel":"kline",
  "symbols":["BTC_USDT"],
  "market_type":"futures"
}}
```

| Field | Required | Meaning |
|---|---|---|
| `channel` | no | Must be `kline` if present (default `kline`). |
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

### `subscribe_orders` — channel `order`

```json
{"jsonrpc":"2.0","id":2,"method":"subscribe_orders","params":{
  "market_type":"cfd",
  "symbols":["XAUUSD"]
}}
```

Same parameter shape as `subscribe_market`; `channel`, if present, must be
`order`. The reply carries `"channel":"order"` and the note `"notifications
are pushed for submitted and completed order events"`.

### `unsubscribe_market` / `unsubscribe_orders`

```json
{"jsonrpc":"2.0","id":3,"method":"unsubscribe_orders"}
```

Each clears **its own channel only** and is idempotent: clearing an absent
subscription succeeds, the reply is
`{"subscribed":false,"channel":"order"}`. Subscriptions on the other channel
survive. Re-subscribing to a channel replaces that channel's filter.

## Notifications

The server initiates these; they carry **no `id`**.

### `market.kline`

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

### `order.update`

Two event kinds share the channel. `ts` is epoch milliseconds.

`submitted` — emitted once per order that actually reached the exchange
through `open_order`. Rejected requests are not announced, and neither is a
replayed idempotent retry (nothing new was placed). Orders placed internally
by strategies do not emit `submitted`.

```json
{"jsonrpc":"2.0","method":"order.update","params":{
  "event":"submitted",
  "order_id":"51234567",
  "client_order_id":"gt-xau-1",
  "symbol":"XAUUSD","market_type":"cfd",
  "side":"buy","type":"limit",
  "price":4025.0,"quantity":0.01,
  "filled_quantity":0.0,
  "status":"open",
  "ts":1794700000000
}}
```

`completed` — emitted once per terminal report the order flow observes (fill
or cancellation). `market_type` is the reporting tracker's own market type.

```json
{"jsonrpc":"2.0","method":"order.update","params":{
  "event":"completed",
  "order_id":"51234567",
  "client_order_id":"gt-xau-1",
  "symbol":"XAUUSD","market_type":"cfd",
  "side":"buy","type":"limit",
  "requested_quantity":0.01,
  "filled_quantity":0.01,
  "avg_fill_price":4024.5,
  "fees":0.0,
  "status":"filled",
  "ts":1794700060000
}}
```

Field notes:

- `side` is `buy` / `sell`; `type` is `market` / `limit` / `post_only` /
  `maker_first`; `market_type` is `spot` / `futures` / `cfd`.
- `status` uses the engine's shared serializer: `pending`, `open`, `filled` or
  `cancelled` (a partially filled live order reports `open`, a rejected order
  reports `cancelled`). On `submitted` the value is **never terminal**: a
  market order's POST reply can already be filled, but the event is emitted
  before the fill is processed, so a terminal status is reported as `open`
  and `filled_quantity` is always `0.0`. The terminal state arrives with
  `completed`.
- `price` is the requested limit price (0 for a market order); `quantity` on
  `submitted` is the size the order flow actually sent — after any engine-side
  risk adjustment applied, so `engine`-authority orders that were shrunk to
  fit a budget report the shrunk size (matching `completed.requested_quantity`).
- `completed` covers orders tracked through the order flow. The dedicated CFD
  position-close endpoint and externally detected closes (manual app close,
  exchange-side stop) are recorded internally but **not** pushed — reconcile
  those through `get_positions` / `get_orders`.

## Semantics and guarantees

- **At-most-once.** There is no replay and no acknowledgement. After a
  reconnect, backfill: `get_market` (`klines` parameter) or `kline_bars` from
  SQLite for bars; `get_orders` / `get_positions` for order state.
- **Closed bars only.** Strategies never see a half-formed candle. This is the
  B6 fix: the pre-2026-11-14 feed marked every push `closed=true`.
- **Interval.** 1 minute. Coarser intervals are not buffered by the engine.
- **Ordering.** Per channel and per symbol, ascending event time. No
  cross-symbol or cross-channel ordering guarantee; the two channels share one
  FIFO queue per connection, so their relative order is publish order.
  `submitted` is published before the order's `completed`, including when a
  market order fills the moment it is placed: both events are produced inside
  the placement call, in that order.
- **Reply before notification.** A response to a request is always written
  before any notification published while that request was being handled. The
  connection is still line-oriented, so a client MUST demultiplex by shape,
  not by position: a line carrying `id` (or `result`/`error`) is a response,
  a line carrying `method` is a notification. The `call()` helper in the
  example below reads one line per request; a subscription-carrying client
  must read continuously and match responses by `id`.
- **Delivery scope.** A `Topic` matches a session when the channel matches,
  the session's `market_type` is empty or equal, and the session's symbol set
  is empty or contains the symbol.

## Backpressure

Each subscriber owns a bounded queue of 1024 notifications. When a client
falls behind, the **oldest** notification is dropped so the freshest event
survives. `publish()` only enqueues, under the subscriber's own mutex, so a
stalled or dead client can never block the market-data hot path or an order
call.

A client that is gone is detected on the next write; the session is marked
stopped and removed from the subscriber list.

## Example (Python)

```python
import json, socket

sock = socket.create_connection(("127.0.0.1", 8081))
f = sock.makefile("rwb")

def call(obj):
    # Safe before any subscription exists. Once subscribed, read continuously
    # and match responses by "id": notifications carry "method" and no "id".
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    return json.loads(f.readline())

print(call({"jsonrpc": "2.0", "id": 1, "method": "subscribe_market",
            "params": {"symbols": ["BTC_USDT"], "market_type": "futures"}}))
print(call({"jsonrpc": "2.0", "id": 2, "method": "subscribe_orders",
            "params": {"market_type": "futures"}}))

while True:
    msg = json.loads(f.readline())
    if msg.get("method") == "market.kline":
        bar = msg["params"]
        print("bar", bar["symbol"], bar["open_time"], bar["close"])
    elif msg.get("method") == "order.update":
        upd = msg["params"]
        print("order", upd["event"], upd["order_id"], upd["status"])
```

If `[control] token` is set, send the auth line first and wait for its reply:

```python
call({"jsonrpc": "2.0", "id": 0, "method": "auth",
      "params": {"token": "<token>"}})
```

## Limitations

- The subscriber is tied to its connection: closing the socket ends every
  subscription on it.
- No authentication beyond the connection-level `auth` line, no per-channel
  access control — a subscribed client sees every order event of the markets
  it matched.
- MCP clients cannot subscribe; poll `get_orders` (fills surface within ~3 s
  via the throttled tracker reconcile).
