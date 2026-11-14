# External strategy API — the contract for an outside runtime

An external strategy runtime (for example the gold decision system, GT) can
trade the same Gate.io account as pulseTrader without embedding its logic in
the engine. It talks to the control socket — newline-delimited JSON-RPC 2.0
over TCP `127.0.0.1:8081` (`[control]` TOML section, `PULSE_CONTROL_PORT`
override) — and receives order events over the same connection.

The engine stays the single writer to the account: one engine holds
`data/engine.lock`, and the runtime never sends orders straight to Gate.io, so
the engine's risk gates, position bookkeeping and exchange-native protective
stops still apply. This page describes the four contracts the runtime must
honour and the two push channels it can use. Transport details for the push
channels are in [control-push.md](control-push.md).

> **MCP cannot receive pushes.** MCP is request/response. The
> `subscribe_market` / `subscribe_orders` pairs are control-socket-only; an
> MCP client must poll `get_orders` / `get_positions` instead.

## The four contracts

### 1. `client_order_id` — idempotent submission

An order is identified by the pair (`client_order_id`, normalized parameters:
symbol, side, quantity, type, price, market_type, leverage, reduce_only,
`sl_price`, `tp_price`).

- The same identity submitted again never reaches the exchange twice. A
  finished first attempt is replayed verbatim — the same `order_id` on success,
  the same error on rejection. An attempt that is still executing is refused as
  a duplicate.
- Reusing a key with different parameters is refused; it is never silently
  treated as the same order.
- `size_authority: "caller"` requires a non-empty `client_order_id`. Only a
  **futures** `reduce_only` request is accepted: futures is the only market
  whose order body sends the flag to Gate, so the exchange bounds the order by
  the open position. `open_order` **rejects** `reduce_only` on spot/CFD — the
  flag would never reach the exchange there and would only exempt the order
  from local risk checks while still opening exposure. Close spot/CFD positions
  with `close_position`.
- The cache is in memory with a 600 s TTL: the guarantee spans a client
  timeout, not an engine restart. Persist the id locally before sending and
  reuse it on every retry.
- A definitive rejection is replayed verbatim. A transport failure that may
  have reached the exchange (timeout, dropped connection, HTTP 5xx) is replayed
  as code `9107` (`ControlOrderOutcomeUnknown`) — **not** as a rejection:
  reconcile `get_orders` / `get_positions` (and `list_futures_orders`) for the
  key before doing anything else, never assume the order was not placed. A
  pre-flight failure (rate limit, drawdown halt, notional/position limits,
  missing executor) leaves the key retryable, so the same id can be resent
  once the condition clears.
- Requests without a key keep the historical fire-and-forget behaviour: one
  call, one order.

### 2. `size_authority` — who decides the quantity

| Value | Behaviour |
|---|---|
| `"engine"` (default) | The risk layer may shrink `quantity` to fit a budget; the fill can be smaller than requested. |
| `"caller"` | `quantity` is authoritative. The risk layer may reject the order, but never silently reduces it. |

Use `"caller"` when the runtime derives size from its own stop distance and
R-multiple: a silent reduction would invalidate both. Use `"engine"` for
operator one-shots and engine-managed signal orders.

### 3. `[control] external_orders_enabled` — the runtime kill switch

Caller-authoritative, non-reduce-only opens are rejected while the gate is
false (the fail-safe default):

```toml
[control]
external_orders_enabled = true
```

The gate is independent of `[strategy] signal_only`: PT strategies can stay
signals-only while an external runtime trades. Toggle it at runtime, without an
engine restart, via `set_external_orders` (also an MCP tool):

```json
{"jsonrpc":"2.0","id":5,"method":"set_external_orders","params":{"enabled":true}}
```

While off, those opens fail with `ControlInvalidRequest` and a message naming
the TOML key. Futures `reduce_only` closes and engine-authority orders stay
live, so a position can always be flattened (spot/CFD closes use
`close_position`). A refused request does not consume its `client_order_id`, so
the same request succeeds once the gate is enabled. `get_status` reports the
live value; a restart returns to the TOML value.

### 4. Origin and exit ownership — including the restart caveat

Each position carries an in-memory `origin` that decides who owns the exit:

| Origin | Meaning | Exit owner |
|---|---|---|
| `"external"` | Filled from a `size_authority: "caller"` order | The caller (GT/runtime) **while the exchange-native floor exists** — a CFD position with `sl_price` > 0. PT's L7 GoldTrader USD ladder and ProtectiveExitManager skip exactly that case. A CFD entry without `sl_price`, or any futures/spot external position, has no exchange-side stop, so PT's exit managers keep managing it rather than leave it unprotected. |
| `"strategy"` | Any non-caller-authoritative order carrying a `client_order_id` (the signal path always mints one) | PT |
| `"manual"` | Keyless one-shot, or an exchange position imported by sync | PT |

The runtime's safety floor is the exchange-native stop attached at entry (CFD
`sl_price` / `tp_price`), which fires even if both the runtime and the engine
are down. Attach `sl_price` to every caller-authoritative CFD entry — without
it the position is treated as unprotected and PT applies its own exit logic.
Futures and spot orders cannot carry that stop (`open_order` rejects
`sl_price`/`tp_price` for them), so their external positions stay PT-managed.

**Restart caveat.** `origin` is not persisted. After an engine restart,
`sync_positions` re-imports the exchange position as `"manual"` — the exchange
payload carries no client tag — so a position that was `"external"` becomes
PT-managed again until its owner re-establishes ownership. The runtime must
reconcile at startup (see below) and, if PT management must be avoided, flatten
and re-enter with `size_authority: "caller"`; until then the exchange-native
stop is the only floor. A restart also empties the idempotency cache: an id the
runtime kept locally is a new identity to the engine.

## Push subscriptions

`subscribe_market` (channel `kline`, `market.kline`, closed 1-minute bars) and
`subscribe_orders` (channel `order`, `order.update`, order lifecycle) can both
be held on one connection — one subscription per channel, so subscribing the
second never replaces the first. Filters are optional: `market_type`
(`spot` / `futures` / `cfd`) and `symbols` (array of symbols); omitted or empty
means wildcard. `unsubscribe_orders` clears only the `order` channel.

```json
{"jsonrpc":"2.0","id":1,"method":"subscribe_orders","params":{
  "market_type":"cfd","symbols":["XAUUSD"]
}}
```

`order.update` carries two events:

- `submitted` — once per `open_order` that actually reached the exchange.
  Rejected requests and replayed idempotent retries emit nothing. It is
  published **before** that order's `completed`, including for a market order
  that fills on submission; `quantity` is the size actually sent (after any
  engine-side risk adjustment) and the status is never terminal here
  (`filled_quantity` is always 0 on this event).
- `completed` — once per terminal report observed by the order flow (fill or
  cancellation), tagged with the reporting tracker's own market type.

A connection carries both responses and notifications. A response is written
before any notification published while its request was being processed, but
notifications interleave between replies — demultiplex by shape: `id` (plus
`result`/`error`) marks a response, `method` marks a notification. Never
assume the next line is the answer to the request you just sent.

```json
{"jsonrpc":"2.0","method":"order.update","params":{
  "event":"completed","order_id":"51234567",
  "client_order_id":"gt-20261114-0001",
  "symbol":"XAUUSD","market_type":"cfd",
  "side":"buy","type":"limit",
  "requested_quantity":0.01,"filled_quantity":0.01,
  "avg_fill_price":4024.5,"fees":0.0,
  "status":"filled","ts":1794700060000
}}
```

Push is **at-most-once**: no acknowledgement, no replay, no gap marker. Each
connection owns a bounded queue of 1024 notifications; a client that falls
behind has its OLDEST event dropped. Treat notifications as low-latency hints
and let the reconciliation loop be the source of truth.

A CFD `close_position` uses exchange-native position endpoints, not an order,
so it is recorded internally but **not** pushed as `completed` — confirm it
through `get_positions`. Futures closes do flow through the order tracker and
do emit `completed`.

## Recommended reconciliation loop

1. **Connect and backfill.** Authenticate if a token is configured, send both
   subscriptions, then snapshot `get_positions` and `get_orders`.
   `get_orders` reconciles the trackers before answering. For futures, also
   call `list_futures_orders` per contract: it returns orders the trackers do
   not know about (App-side, pre-restart leftovers).
2. **Maintain local order state** keyed by `client_order_id`. Apply
   `submitted` / `completed` events, but never assume one arrived.
3. **Poll periodically** (every 30–60 s) with `get_orders` and
   `get_positions`, and treat exchange state as authoritative.
   `sync_positions` also runs at startup and every ~10 s, pruning positions
   the operator closed manually in the Gate app.
4. **After a reconnect**, re-subscribe and repeat step 1. Missed events are
   not replayed.
5. **After an engine restart**, re-check `get_status`
   (`external_orders_enabled` returns to the TOML value) and re-establish
   ownership of any position that was opened with `size_authority: "caller"`.
6. **On any ambiguous reply** (timeout, dropped socket), retry `open_order`
   with the same `client_order_id` — replay makes the retry safe. Never mint a
   fresh id for a retry. A replay whose code is `9107`
   (`ControlOrderOutcomeUnknown`) means the first attempt may have reached the
   exchange: reconcile before resubmitting. A `9108` (`ControlBusy`) replay is
   transient backpressure — retry the same id shortly.

## Small examples

Place a caller-sized CFD order with an attached stop:

```json
{"jsonrpc":"2.0","id":10,"method":"open_order","params":{
  "symbol":"XAUUSD","market_type":"cfd","side":"buy","type":"limit",
  "quantity":0.01,"price":4025.0,"leverage":500,
  "client_order_id":"gt-20261114-0001",
  "size_authority":"caller","sl_price":4012.0
}}
```

```json
{"jsonrpc":"2.0","id":10,"result":{
  "order_id":"51234567","status":1,"submit_time":1794700000000
}}
```

`status` in the reply is the engine's integer order-status enum; the push
event carries the string form (`pending` / `open` / `filled` / `cancelled`).
A rejected call returns a JSON-RPC error whose `data.code` is the `PulseError`
code and whose message names the contract that failed.

Close a position by its engine id (CFD closes are confirmed through
`get_positions`, not `order.update`):

```json
{"jsonrpc":"2.0","id":11,"method":"close_position","params":{
  "position_id":"XAUUSD_Buy_1","quantity":0.01
}}
```
