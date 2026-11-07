# pulseTrader Qt Client — Architecture and Design

| Field | Value |
| ----- | ----- |
| Status | Draft — implementation-ready design |
| Branch | `qt-client` |
| Scope | Read-only monitoring client for Windows, macOS, Ubuntu, Android |
| UI stack | Qt 6.12.0 LTS / QML / Qt Quick (decided) |
| Engine contract | L9 control socket: newline-delimited JSON-RPC 2.0 over TCP |
| Location | `client/qt/`, standalone CMake project, not built by the top level by default |
| Last updated | 2026-11-07 |

Contents: [1 Goals](#1-goals--non-goals) · [2 Wire contract](#2-wire-contract-summary) · [3 Methods](#3-method-contracts) · [4 Project and CMake](#4-qt-project-layout-and-cmake) · [5 TLS and token](#5-tls--token-design-for-the-control-socket) · [6 Client architecture](#6-client-architecture) · [7 Milestones](#7-milestones) · [8 Risks](#8-risks-and-open-questions)

---

## 1. Goals / Non-goals

### 1.1 Goals

| ID | Goal |
| -- | ---- |
| G-1 | One source tree produces a native client for Windows, macOS, Ubuntu and Android. |
| G-2 | The client monitors a running pulseTrader engine over the existing L9 control socket — no new engine API in v1. |
| G-3 | First release is **read-only**: `get_status`, `get_account`, `get_positions`, `get_orders`, `get_signals`, `get_market` only. |
| G-4 | Remote use is possible through an **optional** TLS + token layer on the control socket; default engine behavior (loopback, plaintext, no auth) is byte-for-byte unchanged. |
| G-5 | The client is a standalone CMake project under `client/qt/`; the top-level build ignores it unless `-DPULSE_BUILD_QT_CLIENT=ON`. |
| G-6 | The client degrades honestly: missing data is shown as unknown, never as zero; stale data is visibly stale; a dead engine never freezes the UI. |
| G-7 | The client never links engine code and never builds engine dependencies (nlohmann_json, spdlog, asio, OpenSSL, CURL) on any platform, including Android. |

### 1.2 Non-goals (v1)

| ID | Non-goal | Rationale |
| -- | -------- | --------- |
| N-1 | Order placement, cancellation, position close, `halt_trading`/`resume_trading`, strategy parameter writes, grid control, trigger orders. | Scope decision. The socket exposes all 32 methods (`src/control/JsonRpcServer.cpp:366-588`); the client deliberately implements six. |
| N-2 | Server-initiated push, subscriptions, streaming. | The engine has no such mechanism; the only server write is the direct reply to the line just read (`src/control/JsonRpcServer.cpp:341-345`). Adding push is an engine project, not a client project. |
| N-3 | MCP transport. | MCP is a stdio adapter over the same registry (`src/control/McpServer.cpp:692-730`). The client speaks TCP JSON-RPC directly. |
| N-4 | Reuse of `src/control/ControlClient`. | Blocking asio, per-call local read buffer, no id correlation (`src/control/ControlClient.cpp:156-181`), and it links the entire engine stack (`src/control/CMakeLists.txt:26-40`), which does not exist for Android ABIs. |
| N-5 | Qt Charts / candlestick charting. | Qt Charts is not installed on this host (Ubuntu candidate `qt6-charts-dev 6.10.2-1`, not installed) and is a separate add-on in the Qt 6.12 Android kit (`qt.qt6.6120.addons.qtcharts.android_arm64_v8a`, 0.7 MB, not required by the base package). v1 ships a QML `Canvas` sparkline at most. |
| N-6 | Local database, trade history beyond what `get_orders` returns, offline mode. | Not needed for monitoring. |
| N-7 | Qt WebEngine. | Not an Android target; no use case here. |

### 1.3 Fixed decisions (do not re-litigate)

- UI stack is QML / Qt Quick on Qt 6.12.0 LTS (released 2026-09-30; last Qt release supporting Windows 10).
- TLS + token are **optional additions** to the control socket; both defaults off.
- `client/qt/` is a standalone CMake project, not built by the top level by default.
- First release scope is the six read-only methods above.

---

## 2. Wire contract summary

### 2.1 Transport and framing

| Property | Value | Evidence |
| -------- | ----- | -------- |
| Transport | Plain TCP; `asio::ip::tcp::acceptor`/`socket` | `src/control/JsonRpcServer.cpp:176-207` |
| Default endpoint | `127.0.0.1:8081`, enabled by default | `src/core/config.hpp:243-247` |
| Framing | One JSON object per line; response written as `dump() + "\n"` | `src/control/JsonRpcServer.cpp:341-345` |
| Read strategy | `asio::read_until(sock, buffer, '\n')`, then `buffer.consume(n)` | `src/control/JsonRpcServer.cpp:316-328` |
| CRLF | Tolerated; all trailing `\r`/`\n` characters stripped | `src/control/JsonRpcServer.cpp:330-339` |
| Empty line | Ignored, no reply | `src/control/JsonRpcServer.cpp:337-340` |
| Pipelining | Bytes past the first `\n` stay buffered for the next iteration | `src/control/JsonRpcServer.cpp:316-328` |
| Frame size limit | **None.** `asio::streambuf` default `maximum_size` is `SIZE_MAX` | `src/control/JsonRpcServer.cpp:316`; `asio/basic_streambuf.hpp:131-132` |
| Batch | Not supported: a top-level array is `-32600` | `src/control/JsonRpcServer.cpp:87-95` |
| Server push | None; poll only | `src/control/JsonRpcServer.cpp:341-345` |

There is no `Content-Length`, no length prefix, no HTTP framing, and no application-level maximum line size. A client that sends a line with no `\n` grows the server streambuf until memory is exhausted. **The client must impose its own caps** (Section 2.6, C-9).

### 2.2 Request envelope

A request is a single JSON object per line:

```json
{"jsonrpc":"2.0","id":1,"method":"get_status","params":{}}
```

| Field | Required | Notes |
| ----- | -------- | ----- |
| `jsonrpc` | No | Never validated by the server. Always send `"2.0"` for hygiene. |
| `method` | **Yes** | Must be a JSON string. Case-sensitive; matched against the registry. |
| `params` | No | Defaults to `{}` when absent. Passed verbatim to the handler. |
| `id` | **Yes in practice** | Any JSON type; echoed. Absent **or `null`** makes the request a **notification**: the handler still executes (side effects included) but no reply is ever sent. |

Validation is `!req.is_object() || !req.contains("method") || !req["method"].is_string()` (`src/control/JsonRpcServer.cpp:87-97`). The `jsonrpc` field appears only in response construction, never in request validation.

`params` may be any JSON type at the dispatcher level (`src/control/JsonRpcServer.cpp:101-102`), but handlers call `nlohmann::json::value(...)`, which requires an object and throws otherwise (`json.hpp:2226-2241`). The dispatcher converts the throw into `-32603` with `data.code` 9103, message `handler exception: ...` (`src/control/JsonRpcServer.cpp:122-137`). The control socket therefore **never emits `-32602`**.

### 2.3 Response envelope

Success:

```json
{"jsonrpc":"2.0","id":1,"result":{ ... }}
```

Failure:

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32603,"message":"...","data":{"code":9101,"message":"..."}}}
```

- Responses are compact (`json.dump()`, no whitespace) and terminated by exactly one `\n` (`src/control/JsonRpcServer.cpp:139-156`, `:344`).
- The `id` is copied with `resp["id"] = req["id"]` — type is not validated; for `-32700` and `-32600` the id is `null` (`src/control/JsonRpcServer.cpp:79-95`).
- Object keys are **alphabetically sorted**, because the engine uses default `nlohmann::json` (std::map-backed) everywhere and no `ordered_json` exists under `src/`. Never assert on key order or do byte-level golden comparisons.

### 2.4 Error model on the TCP socket

Exactly four JSON-RPC codes exist (`grep -c 32602 src/control/JsonRpcServer.cpp` = 0):

| JSON-RPC code | Message | `error.data` | Trigger |
| ------------- | ------- | ------------ | ------- |
| `-32700` | `Parse error` | absent | Unparseable JSON. Connection survives. |
| `-32600` | `Invalid request` | absent | Non-object body, missing/non-string `method`, or top-level array. |
| `-32601` | `Method not found` | `{"code":9102}` | Method not in the registry (`src/control/JsonRpcServer.cpp:104-120`). |
| `-32603` | `<PulseError message>` | `{"code":<PulseError>,"message":<same>}` | Every handler failure and every handler exception (wrapped as 9103 with prefix `handler exception: `). |

`data.code` is **an open enum**, not a closed set: 91xx control/grid, 92xx grid service, 1xxx network, 2xxx exchange, 3xxx risk, 7xxx futures, 71xx CFD (`src/core/PulseError.hpp:98-121`; `src/control/EngineServices.cpp` order/trigger paths). A client must key user-facing text on `message` and only branch structurally on a small allow-list of codes.

Two methods report logical failure **inside a successful result** rather than as an error object: `switch_direction` (`src/control/EngineServices.cpp:881-916`) and `get_market` (`src/control/EngineServices.cpp:517-539`). Of the six read-only methods, only `get_market` does this. The in-band value is a **string** at `result.error`, not the JSON-RPC `error` object. The reference `ControlClient` checks only the envelope and therefore passes these through as successes (`src/control/ControlClient.cpp:166-181`).

### 2.5 Concurrency and liveness

| Property | Value | Evidence |
| -------- | ----- | -------- |
| Server threads | One accept thread + **one blocking thread per connection** | `src/control/JsonRpcServer.cpp:202`, `:287-292` |
| Per-connection ordering | Strictly sequential: read → dispatch → write | `src/control/JsonRpcServer.cpp:317-346` |
| Cross-connection ordering | Parallel | same |
| Connection cap | None (`backlog = max_listen_connections`) | `src/control/JsonRpcServer.cpp:186` |
| Idle / read timeout | **None**; a silent client parks a thread forever | `src/control/JsonRpcServer.cpp:312-347` |
| Request timeout | **None** | same |
| Session bookkeeping | `std::thread` stays `joinable()` after its function returns, so the `!t.joinable()` purge can never erase anything; `m_sessions`/`m_sessionSockets` grow until `stop()` | `src/control/JsonRpcServer.cpp:292-302`; `:250-258` |
| Malformed input | Connection survives; `-32700` then continue | `src/control/JsonRpcServer.cpp:316-347` |
| Transport failure | Silent close; never an error frame | `src/control/JsonRpcServer.cpp:348-359` |

Consequence: a slow handler blocks every later request **on that connection only**. `get_account` can block for tens of seconds; `get_positions`/`get_orders` perform REST reconciliation. The client must not multiplex them onto one socket (Section 6.6).

### 2.6 Constraints these impose on the client

| ID | Rule | Reason | Evidence |
| -- | ---- | ------ | -------- |
| C-1 | Always send a non-null `id`; never use notification form. | Missing/null id = no reply, handler still runs. | `src/control/JsonRpcServer.cpp:99-100`, `:134-137` |
| C-2 | Write exactly one `\n` per request; never send `\r`. | Canonical framing. | `src/control/JsonRpcServer.cpp:319`, `:330-339` |
| C-3 | Never send a top-level JSON array. | Batch is rejected `-32600`. | `src/control/JsonRpcServer.cpp:87-95` |
| C-4 | Always send `params` as an object. | `.value()` on an array throws → 9103. | `src/control/JsonRpcServer.cpp:101-102`, `:122-137` |
| C-5 | Parse by key only; never assume key order. | std::map-backed JSON. | `src/control/JsonRpcServer.cpp:139-156` |
| C-6 | Never use array index as identity. | unordered_map iteration; positions/orders unordered. | `src/risk/PositionManager.cpp:470-482`; `src/execution/OrderTracker.cpp:775-798` |
| C-7 | Correlate replies by `id`, even though the server answers in order. | Detects desync; the reference client does not. | `src/control/ControlClient.cpp:156-181` |
| C-8 | Enforce a client-side timeout on every request. | Server has none. | `src/control/JsonRpcServer.cpp:312-347` |
| C-9 | Cap request and response line size (64 KiB / 1 MiB). | Server bound is `SIZE_MAX`. | `src/control/JsonRpcServer.cpp:316` |
| C-10 | Use a small fixed set of long-lived connections; no connection churn. | Thread-per-connection, never reaped. | `src/control/JsonRpcServer.cpp:287-302` |
| C-11 | Poll on a schedule; never wait for server push. | No subscription mechanism. | `src/control/JsonRpcServer.cpp:341-345` |
| C-12 | Check `result.error` on `get_market`. | In-band logical error. | `src/control/EngineServices.cpp:517-539` |
| C-13 | Treat `data.code` as an open enum; display `message`. | Deeper layers propagate their own codes. | `src/core/PulseError.hpp:98-121` |
| C-14 | Treat `-32601` / `data.code` 9102 as "capability absent". | No version negotiation exists. | `src/control/JsonRpcServer.cpp:104-120` |

---

## 3. Method contracts

All six handlers ignore their `params` value except `get_market`; they are lambdas with an unnamed parameter (`src/control/JsonRpcServer.cpp:371-386`, `:426-429`). MCP declares the same six with empty property sets, except `get_market`, whose MCP schema marks `symbol` required (`src/control/McpServer.cpp:196-216`, `:443-460`). Values in the examples below are illustrative; key sets and types come from the cited code. The raw epoch fields and their `*_str` renderings are independent illustrations and do not correspond to each other — regenerate both from the same instant before using them as test fixtures.

### 3.0 Shared rules

- **Numbers** are JSON doubles unless stated integer; no fixed-point or string-encoded amounts are used, except Gate's raw REST strings which the engine already parses.
- **Timestamps**: `*_str` fields are `YYYY-MM-DDTHH:MM:SS.mmm±HH:MM` in the engine's configured display timezone (`[control] display_timezone`: `local` default, `utc`, or `±HH:MM`) (`src/core/TimeUtil.hpp:136-195`; `src/core/config.hpp:249-256`). Raw epoch fields are machine-readable and timezone-independent. **Render `*_str` verbatim**; use the raw epoch only to compute age.
- **Absence means unknown**, never zero. Blocks are omitted when the underlying client is missing or the call failed.

### 3.1 `get_status`

**Params:** none. Any params value is ignored.

**Result:** all keys always present except the `feed_*` groups.

| Key | Type | Meaning | Source |
| --- | ---- | ------- | ------ |
| `version` | string | Engine version. Passed as the literal `"0.1.0"` at startup. | `apps/pulsetrader/main.cpp:1247`; `src/control/EngineServices.cpp:184` |
| `uptime_sec` | integer | Whole seconds since the engine-start timestamp captured just before control-plane construction (steady clock), not since `EngineServices` construction. | `src/control/EngineServices.cpp:180-183`; `apps/pulsetrader/main.cpp:1211`, `:1246` |
| `network` | string | `"mainnet"` or `"testnet"` from `exchange.testnet`. | `src/control/EngineServices.cpp:186` |
| `symbols` | string[] | Top-level `symbols` config; may be `[]`. | `src/control/EngineServices.cpp:187` |
| `strategies_running` | integer | Strategies whose cooperative run flag is true. A **paused** strategy still counts. | `src/strategy/StrategyManager.cpp:135-149`; `src/strategy/StrategyBase.hpp:88-93` |
| `strategies_total` | integer | Enabled config instances only; disabled instances are skipped before registration. | `apps/pulsetrader/main.cpp:930-934`; `src/strategy/StrategyManager.cpp:135-149` |
| `open_positions` | integer | `PositionManager` map size. | `src/risk/PositionManager.hpp:227` |
| `trading_halted` | bool | Risk manager halt flag. | `src/control/EngineServices.cpp:192` |
| `active_market` | string | `"spot"` \| `"futures"` \| `"cfd"`. | `src/control/EngineServices.cpp:193`; `src/control/OrderFlowExecutor.hpp:301` |
| `feed_spot`, `feed_futures`, `feed_cfd` | object | **Present only when that market feed exists.** `{ticker_count, orderbook_count, kline_count}`, monotonic since feed start. Absence means "no feed wired for that market", not "zero events". | `src/control/EngineServices.cpp:194-205`; `src/core/snapshot_types.hpp:84-92`; `apps/pulsetrader/main.cpp:705-756` |

**Example** (alphabetical key order, as emitted):

```json
{"jsonrpc":"2.0","id":1,"result":{"active_market":"futures","feed_futures":{"kline_count":4210,"orderbook_count":180042,"ticker_count":960010},"network":"mainnet","open_positions":1,"strategies_running":2,"strategies_total":2,"symbols":["BTC_USDT"],"trading_halted":false,"uptime_sec":3725,"version":"0.1.0"}}
```

**Client notes:** cheapest method — pure in-memory reads. Poll at 1 s. `strategies_running` is not "unpaused"; label it "enabled" in the UI, not "active". Never derive fresh position state from `open_positions`; use `get_positions`.

### 3.2 `get_account`

**Params:** none.

**Result:** `available` always present; other keys conditional.

| Key | Type | Presence | Meaning |
| --- | ---- | -------- | ------- |
| `available` | bool | always | **`false` does not mean zero balance.** It means: no futures REST client configured, or the futures balance call failed (`src/control/EngineServices.cpp:211-231`). |
| `total` | number | only when `available:true` | Futures account equity. |
| `available_balance` | number | only when `available:true` | Rename of `AccountBalance.available`. |
| `unrealised_pnl` | number | only when `available:true` | Unrealised PnL. |
| `position_margin` | number | only when `available:true` | Margin held by positions. |
| `order_margin` | number | only when `available:true` | Margin held by orders. |
| `currency` | string | only when `available:true` | Settlement currency, default `"USDT"`. |
| `spot` | object | only when a spot REST client exists **and** the call succeeds | `{available:number, total:number, currency:"USDT"}`; `total = available + locked` for the first USDT row. |
| `cfd` | object | only when a CFD REST client exists **and** the call succeeds | `{total, available, margin, unrealised_pnl, currency:"USD"}`, read from Gate tradfi fields `equity`/`margin_free`/`margin`/`unrealized_pnl`. |

Sources: `src/control/EngineServices.cpp:209-261`; spot mapping `:78-105`; `src/exchange/GateRestClient.cpp:551-558`, `:613-630`, `:426-433`.

**Example:**

```json
{"jsonrpc":"2.0","id":3,"result":{"available":true,"available_balance":512.34,"cfd":{"available":0.0,"currency":"USD","margin":0.0,"total":0.0,"unrealised_pnl":0.0},"currency":"USDT","order_margin":0.0,"position_margin":44.15,"spot":{"available":250.0,"currency":"USDT","total":250.0},"total":556.49,"unrealised_pnl":0.52}}
```

**Client notes:** expensive. It takes the global REST mutex (`std::lock_guard lock(m_restMutex)`, `src/control/EngineServices.cpp:214`) and performs up to three blocking HTTP requests. Each REST call has `restTimeoutMs = 10000` with `maxRetries = 3` (four attempts), so a dark network can hold one client ~40 s and three clients serially longer (`src/core/config.hpp:104-105`; `src/exchange/GateRestClient.cpp:288-317`). Business failures are never JSON-RPC errors; they vanish into the log. The UI must show "unknown" for absent blocks, and `available:false` must never render as `0.00`.

### 3.3 `get_positions`

**Params:** none.

**Result:** exactly two top-level keys.

| Key | Type | Meaning |
| --- | ---- | ------- |
| `positions` | array | Every open position across spot/futures/CFD. Identity is `position_id`; ordering is unspecified. |
| `portfolio` | object | `{net_exposure, openPositionCount, total_notional, total_unrealized_pnl}`. `openPositionCount` is the only camelCase key in the entire object. |

Each `positions[]` element (19 keys; `src/core/snapshot_types.hpp:102-127` plus `src/control/EngineServices.cpp:288-291`):

| Key | Type | Notes |
| --- | ---- | ----- |
| `position_id` | string | Stable identity. |
| `symbol` | string | e.g. `BTC_USDT`, `XAUUSD`. |
| `side` | string | `"buy"` \| `"sell"`. |
| `quantity` | number | Contract count for futures; base-currency units for spot (`quanto = 1`). See risk note below. |
| `entry_price` | number | Quote currency (USDT, or USD for CFD). |
| `current_price` | number | Quote currency. |
| `unrealized_pnl` | number | `direction × (current − entry) × quantity × quanto_multiplier`. |
| `notional_value` | number | Absolute value of `quantity × current_price × quanto_multiplier`. |
| `open_time` | integer | Epoch **milliseconds**, UTC. |
| `open_time_str` | string | Display-timezone ISO-8601 with offset. |
| `strategy_id` | string | Owning strategy. |
| `market_type` | string | `"spot"` \| `"futures"` \| `"cfd"`. |
| `leverage` | number | Leverage; scales margin only, never PnL. |
| `margin_mode` | string | `"cross"` \| `"isolated"`. |
| `margin_used` | number | `quantity × entry × quanto / leverage` (futures). |
| `liquidation_price` | number | 0 when not applicable. |
| `exchange_position_id` | string | `""` for spot/futures; CFD only. |
| `sl_price` | number | `0.0` when no stop. |
| `tp_price` | number | `0.0` when no target. |

Not serialized, although the structs carry them: `quanto_multiplier` on `Position` (`src/risk/risk_types.hpp:101`), and `total_margin_used` / `futures_position_count` on `PortfolioSummary` (`src/risk/risk_types.hpp:145-163`), omitted by the serializer at `src/core/snapshot_types.hpp:130-137`. `maintenance_rate` is **not** stored: it exists only as a `PositionManager` parameter used to compute `liquidation_price` (`src/risk/PositionManager.cpp:40`, `:181`, `:290-296`).

**Example:**

```json
{"jsonrpc":"2.0","id":2,"result":{"portfolio":{"net_exposure":442.05,"openPositionCount":1,"total_notional":442.05,"total_unrealized_pnl":0.5215},"positions":[{"current_price":63150.0,"entry_price":63075.5,"exchange_position_id":"","leverage":10.0,"liquidation_price":56767.95,"margin_mode":"cross","margin_used":44.16,"market_type":"futures","notional_value":442.05,"open_time":1759872000000,"open_time_str":"2025-10-08T00:00:00.000+08:00","position_id":"BTC_USDT_Buy_1","quantity":70.0,"side":"buy","sl_price":0.0,"strategy_id":"s1","symbol":"BTC_USDT","tp_price":41938.0,"unrealized_pnl":0.5215}]}}
```

**Client notes:** `get_positions` calls `reconcileAll()` on all three trackers before snapshotting, which polls each tracked order over REST (`src/control/EngineServices.cpp:268-279`; `src/execution/OrderTracker.cpp:441-464`). Latency grows with tracked-order count; poll at ≥2 s. Do not compute portfolio values yourself from the rows — display the engine's `portfolio` block, and treat a mismatch as an engine-view artifact.

### 3.4 `get_orders`

**Params:** none.

**Result:** exactly two keys, both always present: `activeOrders` (array) and `recentReports` (array). Spot + futures + CFD are merged into both (`src/control/EngineServices.cpp:295-336`).

**`activeOrders[]`** — 11 keys. `OrderSnapshot::to_json` emits 9; the service adds 2 `*_str` companions (`src/core/snapshot_types.hpp:185-201`; `src/control/EngineServices.cpp:341-353`):

| Key | Type | Notes |
| --- | ---- | ----- |
| `order_id` | string | Exchange order id. |
| `symbol` | string | — |
| `side` | string | `"buy"` \| `"sell"`. |
| `type` | string | `"market"` \| `"limit"` \| `"post_only"`. |
| `requested_qty` | number | — |
| `filled_qty` | number | — |
| `status` | string | `"pending"` \| `"open"` \| `"filled"` \| `"cancelled"`. `PartiallyFilled` serializes as `"open"`; `Rejected` serializes as `"cancelled"` (`src/core/snapshot_types.hpp:160-182`). |
| `submit_time` | integer | Epoch **milliseconds**. |
| `last_update_time` | integer | Epoch **milliseconds**. |
| `submit_time_str` | string | Display timezone. |
| `last_update_time_str` | string | Display timezone. |

`client_order_id` exists on the struct (`src/execution/OrderTracker.hpp:32-43`) but is **not emitted** for active orders. It appears only in `recentReports`. A grid/order-correlation UI cannot join active orders by client id — see open question Q-8.

**`recentReports[]`** — 17 keys: 15 from `ExecutionReport::to_json` (`src/execution/ExecutionReport.cpp:10-38`) plus `submit_time_str` and `fill_time_str` (`src/control/EngineServices.cpp:347-354`):

`order_id`, `client_order_id`, `symbol`, `side`, `type` (`market`|`limit`|`post_only`|`maker_first`|`unknown`), `requested_qty`, `filled_qty`, `avg_fill_price`, `submit_mid_price`, `slippage_bps`, `fees`, `latency_ms`, `submit_time` (**nanoseconds**), `fill_time` (**nanoseconds**), `final_status` (`filled`|`cancelled`|`unknown`), `submit_time_str`, `fill_time_str`.

**Critical unit mismatch:** `activeOrders` timestamps are epoch **milliseconds**, `recentReports` timestamps are epoch **nanoseconds**, in the same response. The `*_str` fields are already correct. The service divides reports by `1000000` when formatting, with an explicit comment about the mismatch (`src/control/EngineServices.cpp:338-355`).

At most 20 reports per tracker, so ≤60 entries, sorted by `fill_time` descending **within each tracker block**, not globally (`src/execution/OrderTracker.cpp:833-860`; `src/control/EngineServices.cpp:318-334`).

**Example:**

```json
{"jsonrpc":"2.0","id":7,"result":{"activeOrders":[{"filled_qty":0.0,"last_update_time":1789000000123,"last_update_time_str":"2026-08-19T20:26:40.123+08:00","order_id":"2345678901","requested_qty":0.01,"side":"sell","status":"open","submit_time":1789000000000,"submit_time_str":"2026-08-19T20:26:40.000+08:00","symbol":"ETH_USDT","type":"limit"}],"recentReports":[{"avg_fill_price":4401.2,"client_order_id":"","fees":0.0022,"filled_qty":0.01,"fill_time":1789000000000000000,"fill_time_str":"2026-08-19T20:26:40.000+08:00","final_status":"filled","latency_ms":842,"order_id":"2345678899","requested_qty":0.01,"side":"buy","slippage_bps":0.4544,"submit_mid_price":4401.0,"submit_time":1788999999100000000,"submit_time_str":"2026-08-19T20:26:39.100+08:00","symbol":"ETH_USDT","type":"market"}]}}
```

**Client notes:** also calls `reconcileAll()` on all three trackers (`src/control/EngineServices.cpp:296-310`), so it is not side-effect free and generates exchange REST traffic. Poll at 3 s or slower; never in a tight loop. The list is the engine's local view: orders placed outside the engine do not appear; use `list_futures_orders` on the exchange side if that matters (out of v1 scope).

### 3.5 `get_signals`

**Params:** none.

**Result:** `{signals: [...], aggregate: null | {...}}`. Both keys always present. No I/O; cannot fail.

**`signals[]`** — 10 keys (`src/strategy/signal/SignalBoard.cpp:27-40`, `:77`; `src/control/EngineServices.cpp:367-379`):

| Key | Type | Notes |
| --- | ---- | ----- |
| `source` | string | The `strategy_id`. Anonymous signals are dropped at publish time (`src/strategy/signal/SignalBoard.cpp:49-55`). |
| `symbol` | string | — |
| `market_type` | string | `"spot"` \| `"futures"` \| `"cfd"`. |
| `type` | string | `"buy"` \| `"sell"` \| `"flat"`. |
| `confidence` | number | 0..1 by convention. |
| `price` | number | Signal price. |
| `reason` | string | Free text from the strategy. |
| `ts_ms` | integer | Epoch ms of the **board publish**, not `TradingSignal.timestamp`. |
| `indicators` | object | Arbitrary JSON supplied by the strategy; default `{}`. |
| `ts_str` | string | Display timezone. |

**`aggregate`** — same schema, `source` hard-coded to `"aggregate"`, plus `threshold` (number, the aggregator's configured value) (`src/strategy/signal/SignalBoard.cpp:79-83`).

The board keeps only the **latest** entry per strategy, sorted lexicographically by `strategy_id` (std::map) (`src/strategy/signal/SignalBoard.hpp:59`; iteration order at `src/strategy/signal/SignalBoard.cpp:75-78`). No history, no paging, no server-side freshness filter.

**Example:**

```json
{"jsonrpc":"2.0","id":8,"result":{"signals":[{"confidence":0.8,"indicators":{"ema_fast":4401.0,"ema_slow":4399.0},"market_type":"cfd","price":4401.5,"reason":"ema cross down","source":"momentum_scalper_XAUUSD","symbol":"XAUUSD","ts_ms":1789000005000,"ts_str":"2026-08-19T20:26:45.000+08:00","type":"sell"}],"aggregate":{"confidence":0.72,"indicators":{},"market_type":"cfd","price":4402.0,"reason":"3/3 consensus","source":"aggregate","symbol":"XAUUSD","threshold":0.7,"ts_ms":1789000005100,"ts_str":"2026-08-19T20:26:45.100+08:00","type":"buy"}}}
```

**Client notes:** freshness is the client's job. The repo's own guidance is to treat entries older than ~120 s as stale (`src/control/McpServer.cpp:213-216`; `docs/OPERATIONAL_GUIDE.md:411`). A strategy that stops publishing leaves its last entry forever — always show age and a stale badge. Do not assume a fixed strategy-id set; render whatever `source` values arrive.

### 3.6 `get_market`

**Params** (`src/control/JsonRpcServer.cpp:467-480`; MCP schema `src/control/McpServer.cpp:443-460`):

| Param | Type | Default | Validation |
| ----- | ---- | ------- | ---------- |
| `symbol` | string | — | **Required.** Missing or empty → `-32603` with `data.code` 9101, message `get_market: symbol is required`. |
| `book_levels` | integer | `5` | Not validated. Passed to `std::size_t`; `0` yields empty arrays; a negative value converts to a huge `size_t` and can throw from `vector::reserve` → `-32603`/9103. |
| `klines` | integer | `0` | Only honoured when `> 0`; otherwise the key is omitted from the result. |
| `market_type` | string | `""` (auto) | Exact-case match only: `spot`/`SPOT`, `futures`/`FUTURES`, `cfd`/`CFD` (`src/core/types.hpp:181-203`). Anything else produces an **in-band** error. |

A JSON float for `book_levels`/`klines` is truncated by `params.value<int>`; a string, `null`, object or array throws and becomes `-32603`/9103 `handler exception: ...`.

**Result:**

| Key | Presence | Schema |
| --- | -------- | ------ |
| `symbol` | always | Echo of the requested symbol. |
| `ticker` | only when the ticker cache holds the symbol | 10 keys: `symbol`, `last`, `bid`, `ask`, `volume_24h`, `change_pct`, `timestamp` (epoch ms), `mark_price`, `index_price`, `funding_rate` (`src/core/snapshot_types.hpp:68-82`). Futures-only fields are `0` for spot. |
| `order_book` | always | `{bids:[{price,quantity}], asks:[...]}`; arrays may be empty (`src/core/snapshot_types.hpp:43-50`). |
| `klines` | only when requested `> 0` | `{open_time, close_time, open, high, low, close, volume, closed}`, ms times, oldest first (`src/core/snapshot_types.hpp:53-65`; `src/market/KlineBuffer.hpp:85-98`). |
| `error` | only on in-band failure | **string**, e.g. `"market_type must be spot/futures/cfd"` or `"no market feed available"`. When present, no `ticker`/`order_book`/`klines`. |

Feed selection: explicit `market_type` wins; otherwise the active trading direction; if that feed is null, futures then spot (`src/control/EngineServices.cpp:511-539`). The response does **not** echo which feed answered.

**Example:**

```json
{"jsonrpc":"2.0","id":9,"result":{"klines":[{"close":63080.1,"close_time":1789000019999,"closed":true,"high":63100.0,"low":63010.5,"open":63020.0,"open_time":1788999960000,"volume":12.345}],"order_book":{"asks":[{"price":63090.5,"quantity":0.42},{"price":63091.0,"quantity":1.1}],"bids":[{"price":63090.0,"quantity":0.35},{"price":63089.5,"quantity":0.8}]},"symbol":"BTC_USDT","ticker":{"ask":63090.5,"bid":63090.0,"change_pct":1.23,"funding_rate":0.0001,"index_price":63085.0,"last":63090.2,"mark_price":63086.0,"symbol":"BTC_USDT","timestamp":1789000006789,"volume_24h":123456.7}}}
```

In-band failure:

```json
{"jsonrpc":"2.0","id":11,"result":{"error":"market_type must be spot/futures/cfd","symbol":"BTC_USDT"}}
```

**Client notes:** always send an explicit lowercase `market_type`, because the response does not identify the feed. Validate `book_levels` in `[1, 50]` and `klines` in `[0, 1000]` before sending. There is no server-side ticker staleness check — `TickerCache::get` returns whatever is cached (`src/market/TickerCache.cpp:16-25`), so compute age from `ticker.timestamp` and show a stale badge past a threshold (suggest 5 s for display, 30 s for "do not trust").

### 3.7 Cost and polling budget

| Method | Cost | Client timeout | Poll interval | Connection |
| ------ | ---- | -------------- | ------------- | ---------- |
| `get_status` | In-memory only | 5 s | 1 s | fast |
| `get_signals` | In-memory only | 5 s | 2 s | fast |
| `get_market` | Feed caches; cost scales with `book_levels` + `klines` | 10 s | 1 s per **visible** symbol | fast |
| `get_positions` | REST reconcile of all tracked orders | 15 s | 2 s | rest |
| `get_orders` | REST reconcile of all tracked orders | 30 s | 3 s | rest |
| `get_account` | Global REST mutex + up to 3 REST calls with retries | 120 s | 30 s | slow |

Never poll methods for pages that are not visible. Pause all polling when the window is minimized (desktop) or the app is backgrounded (Android) and refresh once on resume.

---

## 4. Qt project layout and CMake

### 4.1 Placement and isolation

- The client lives at `client/qt/` in this repository, on the `qt-client` branch. `git ls-files client` returns nothing today — clean slate.
- It is its **own CMake project** (`project()` guarded by `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`) because the root configure hard-requires nlohmann_json, spdlog, fmt, CURL, OpenSSL and toml11 before adding any subdirectory (`CMakeLists.txt:38-43`, `:51-54`), and requires GTest when `PULSE_BUILD_TESTS` is ON (default ON, `CMakeLists.txt:26`, `:56-59`). None of that exists for Android ABIs, so a Qt-for-Android configure of the root tree cannot work.
- The top level gains a default-OFF option only. `apps/CMakeLists.txt:1-2` is the style precedent for a one-line-per-application subdirectory.

### 4.2 Directory layout

```
client/qt/
  CMakeLists.txt                  standalone project; Qt discovery; targets
  CMakePresets.json               linux-debug, linux-release, macos, windows, android-arm64
  main.cpp                        QGuiApplication + QQuickStyle + loadFromModule
  src/
    rpc/
      RpcClient.hpp/.cpp          QAbstractSocket-based JSON-RPC 2.0 transport
      RpcError.hpp/.cpp           error taxonomy (transport / timeout / rpc / in-band)
      RpcMethods.hpp              the six method-name constants
    engine/
      EngineController.hpp/.cpp   QObject facade; owns 3 RpcClients + poll timers
    models/
      PositionsModel.hpp/.cpp     QAbstractListModel, keyed by position_id
      OrdersModel.hpp/.cpp        activeOrders, keyed by order_id
      ReportsModel.hpp/.cpp       recentReports (≤60)
      SignalsModel.hpp/.cpp       keyed by source
      AccountModel.hpp/.cpp       present flags + blocks (not a list)
    util/
      JsonUtils.hpp               QJsonValue <-> QVariant helpers, age/staleness
  qml/
    Main.qml
    Style.qml                     pragma Singleton theme tokens
    pages/
      DashboardPage.qml  PositionsPage.qml  OrdersPage.qml
      SignalsPage.qml    MarketPage.qml     SettingsPage.qml
    components/
      StatusBar.qml  MetricTile.qml  StaleBadge.qml  ErrorBanner.qml
      SignalCard.qml  ConnectionDialog.qml  EmptyState.qml
  resources/
    icons/app.svg
    qtquickcontrols2.conf         only if placed at the resource root (:/qtquickcontrols2.conf)
  android/
    AndroidManifest.xml  build.gradle  gradle.properties
    res/values/strings.xml  res/mipmap-*/ic_launcher.png
  packaging/
    linux/pulsetrader.desktop  linux/pulsetrader.svg  linux/CPackConfig.cmake
    macos/Info.plist.in  macos/entitlements.plist
    windows/pulsetrader.rc  windows/installer.iss
  tests/
    CMakeLists.txt
    tst_rpcclient.cpp             Qt Test against a fake QTcpServer
    tst_jsonmodels.cpp            model diff/keying/staleness tests
    qml/tst_navigation.qml        Quick Test (qmltestrunner)
```

Repo conventions apply inside `client/qt/` too: PascalCase classes, `m_camelCase` members, braces on every `if`/`for`, Yoda comparisons, English-only comments and log messages, filename matching the primary class (`AGENTS.md:77-95`).

### 4.3 `client/qt/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.25)

# Standalone-first: skip project() when the top-level build adds this directory.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(pulseTraderQt VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)

# Sets CMAKE_AUTOMOC and enables QTP0001 (default QML resource prefix /qt/qml).
qt_standard_project_setup(REQUIRES 6.5)

# A pragma Singleton QML file must be marked or qmlcachegen rejects it.
set_source_files_properties(qml/Style.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(pulseTraderQt MANUAL_FINALIZATION
    main.cpp
)

# The backing target is the executable itself, so C++ types declared with
# QML_ELEMENT are registered into the module by listing them under SOURCES.
qt_add_qml_module(pulseTraderQt
    URI Pulse.Client
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Style.qml
        qml/pages/DashboardPage.qml
        qml/pages/PositionsPage.qml
        qml/pages/OrdersPage.qml
        qml/pages/SignalsPage.qml
        qml/pages/MarketPage.qml
        qml/pages/SettingsPage.qml
        qml/components/StatusBar.qml
        qml/components/MetricTile.qml
        qml/components/StaleBadge.qml
        qml/components/ErrorBanner.qml
        qml/components/SignalCard.qml
        qml/components/ConnectionDialog.qml
        qml/components/EmptyState.qml
    SOURCES
        src/rpc/RpcClient.cpp        src/rpc/RpcClient.hpp
        src/rpc/RpcError.cpp         src/rpc/RpcError.hpp
        src/rpc/RpcMethods.hpp
        src/engine/EngineController.cpp src/engine/EngineController.hpp
        src/models/PositionsModel.cpp src/models/PositionsModel.hpp
        src/models/OrdersModel.cpp    src/models/OrdersModel.hpp
        src/models/ReportsModel.cpp   src/models/ReportsModel.hpp
        src/models/SignalsModel.cpp   src/models/SignalsModel.hpp
        src/models/AccountModel.cpp   src/models/AccountModel.hpp
        src/util/JsonUtils.hpp
    RESOURCES
        resources/icons/app.svg
)

target_include_directories(pulseTraderQt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pulseTraderQt PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network)

set_target_properties(pulseTraderQt PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_GUI_IDENTIFIER com.pulsetrader.client
    MACOSX_BUNDLE_BUNDLE_NAME pulseTrader)

# Android properties. They are declared in
# lib/cmake/Qt6Core/Qt6AndroidMacros.cmake, which ships only inside a Qt for
# Android kit: a desktop kit cannot resolve them, and CMake silently ignores an
# undefined target property (the app then keeps the template default).
if(ANDROID)
    set_target_properties(pulseTraderQt PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/android
        QT_ANDROID_PACKAGE_NAME com.pulsetrader.client
        QT_ANDROID_MIN_SDK_VERSION 28)
endif()

include(GNUInstallDirs)
if(NOT ANDROID)
    install(TARGETS pulseTraderQt
        BUNDLE  DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# MANUAL_FINALIZATION keeps the properties above applied before finalization.
# Qt requires qt_finalize_target immediately BEFORE the deploy script is
# generated, not after it (diagnostic in Qt6QmlMacros.cmake).
qt_finalize_target(pulseTraderQt)

if(NOT ANDROID)
    # Install-time deployment: windeployqt on Windows, macdeployqt on macOS.
    qt_generate_deploy_qml_app_script(
        TARGET pulseTraderQt
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${deploy_script})
endif()

option(PULSE_QT_BUILD_TESTS "Build Qt client tests" ON)
if(PULSE_QT_BUILD_TESTS)
    enable_testing()
    # Qt Test and Quick Test are separate CMake packages; without these the
    # tests cannot link and the M-Q2 acceptance criteria have no target.
    find_package(Qt6 6.12 REQUIRED COMPONENTS Test QuickTest)
    add_subdirectory(tests)
endif()
```

`tests/CMakeLists.txt` (not shown in full above) must:

```cmake
qt_add_executable(tst_rpcclient tst_rpcclient.cpp)
target_link_libraries(tst_rpcclient PRIVATE Qt6::Test Qt6::Network pulseTraderQtLib)
add_test(NAME rpcclient COMMAND tst_rpcclient)

# Quick Test: qml/tst_navigation.qml runs under qmltestrunner
qt_add_qml_module(tst_navigation
    URI Pulse.Client.Tests
    VERSION 1.0
    QML_FILES qml/tst_navigation.qml)
```

To make the RPC client testable without an executable target, move the transport and
models into a static library `pulseTraderQtLib` that the executable links; only
`main.cpp` stays in the executable. Without that split, a second target cannot link
the executable.

Notes that matter:

- `qt_finalize_executable` still exists in Qt 6.12 (`Qt6CoreMacros.cmake:1122`) but is only a compatibility alias for `qt_finalize_target`; call the latter.
- `qt_standard_project_setup(REQUIRES 6.5)` turns policy QTP0001 to NEW, so the default QML resource prefix is `/qt/qml`, and `engine.loadFromModule("Pulse.Client", "Main")` works with no import path setup. This is required on Android, where only compiled-in resources are readable.
- **`RESOURCES` entries are prefixed by the module resource path.** With `URI Pulse.Client`, `RESOURCES resources/icons/app.svg` lands at `qrc:/qt/qml/Pulse/Client/resources/icons/app.svg` — **not** `qrc:/resources/icons/app.svg`. A QML `Image { source: "qrc:/resources/..." }` silently renders nothing. Either reference the full prefixed path or give the resource an explicit alias.
- **`qtquickcontrols2.conf` cannot reach `:/qtquickcontrols2.conf` through `RESOURCES`** (same prefixing). If a theme file is needed, add it separately:
  `qt_add_resources(pulseTraderQt "theme" PREFIX "/" BASE resources FILES resources/qtquickcontrols2.conf)`.
  Otherwise drop the file: `QQuickStyle::setStyle("Material")` in `main.cpp` already selects the style, and the rest of the config is optional.
- The "QML source directory must mirror the URI" rule applies to LIBRARY targets, not executable app targets (`Qt6QmlMacros.cmake:541-553`, warning at `:4344`), so a `qml/` subdirectory is safe with one app target.
- Do **not** pass `NO_RESOURCE_TARGET_PATH`. It is legal on Android only because of a special case (`Qt6QmlMacros.cmake:429-435`, `:447-451`); omitting it costs one path segment and removes the dependency on that special case.

### 4.4 Root-tree hook

In `CMakeLists.txt`, add one option next to the existing block at lines 26-28:

```cmake
option(PULSE_BUILD_QT_CLIENT "Build the Qt 6 client under client/qt" OFF)
```

and one guarded subdirectory at the end of the file, after `add_subdirectory(apps)` (line 79):

```cmake
if(PULSE_BUILD_QT_CLIENT)
    add_subdirectory(client/qt)
endif()
```

Default OFF keeps the existing configure path byte-identical. When ON in-tree, the client inherits the global `-Werror -Wall -Wextra -Wpedantic` from `cmake/compiler_flags.cmake:19-24` (included at `CMakeLists.txt:33`), so Qt-generated code must be warning-clean; a standalone configure can set a looser policy.

### 4.5 Per-platform builds

**Ubuntu (Qt 6.12.0 at `$HOME/Qt/6.12.0/gcc_64`, verified):**

Do **not** build against the apt Qt 6.10.2. `cmake --install` fails there with
`RPATH_SET could not write new RPATH ... No valid ELF RPATH or RUNPATH entry exists`
because Debian's QML plugins ship without a RUNPATH, while the Qt-online kit's do.
The same project installs cleanly against `~/Qt/6.12.0/gcc_64`.

```bash
cmake -S client/qt -B build_qt -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/gcc_64
cmake --build build_qt
ctest --test-dir build_qt --output-on-failure
./build_qt/pulseTraderQt
```

**Windows (MSVC 2022 + Qt 6.12.0 msvc2022_64 kit):**

```powershell
cmake -S client\qt -B build_qt -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_PREFIX_PATH=C:\Qt\6.12.0\msvc2022_64
cmake --build build_qt --config Release
C:\Qt\6.12.0\msvc2022_64\bin\windeployqt.exe --release ^
      --qmldir client\qt\qml build_qt\Release\pulseTraderQt.exe
```

Ship the deployed directory (Inno Setup or WiX). `windeployqt` copies Qt DLLs, the platform plugin and QML imports; `--no-compiler-runtime` is only for machines that already have the MSVC runtime.

**macOS (universal binary):**

```bash
cmake -S client/qt -B build_qt -GNinja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/macos
cmake --build build_qt
$HOME/Qt/6.12.0/macos/bin/macdeployqt build_qt/pulseTraderQt.app \
      -qmldir=client/qt/qml -dmg
```

Distribution requires a Developer ID signature and notarization (`-sign-for-notarization=<ident>` implies hardened runtime, timestamp and codesign).

**Android (Qt for Android 6.12.0 kit, JDK 17, NDK r27c — installed and verified on this host):**

```bash
export JAVA_HOME=$HOME/Qt/Tools/OpenJDK-17
export ANDROID_SDK_ROOT=$HOME/Android/Sdk
export ANDROID_NDK_ROOT=$HOME/Android/Sdk/ndk/27.2.12479018

$HOME/Qt/6.12.0/android_arm64_v8a/bin/qt-cmake \
    -S client/qt -B build_qt_android -GNinja \
    -DQT_HOST_PATH=$HOME/Qt/6.12.0/gcc_64 \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-35
cmake --build build_qt_android --target apk      # device testing
cmake --build build_qt_android --target aab      # distribution
```

Verified facts (2026-11-07):

- `qt-cmake` selects the ABI via `-DANDROID_ABI`. `QT_ANDROID_ABIS` is a target property describing what a package was built with, not the selector.
- `QT_ANDROID_MIN_SDK_VERSION` sets the app's minSdkVersion (`Qt6AndroidMacros.cmake:433`); `ANDROID_PLATFORM` selects the NDK platform and is the value r27c caps at 35. Measured in a real build with `-DANDROID_PLATFORM=android-35` and no target property: `gradle.properties` came out `qtMinSdkVersion=28` (Qt default), `qtTargetSdkVersion=37`, `androidCompileSdkVersion=android-36`. The native API level and the manifest SDK levels are independent — compileSdk 36 works even though the native toolchain stopped at 35.
- **NDK r27c caps at API 35.** `-DANDROID_PLATFORM=android-36` aborts with "android-36 is above the maximum supported version 35". API 36 requires NDK r28+.
- Qt 6.12's Android payload targets Android API 28: `lib/libQt6Core_arm64-v8a.so` reports "for Android 28, built by NDK r27c (12479018)".
- The kit is published under `online/qtsdkrepository/all_os/android/qt6_6120/`, **not** `linux_x64/android/` (which stops at 6.7.3). aqtinstall 3.3.0 cannot see this path, so a manual install is required.
- The published archives are build-host artifacts whose `bin/qmake` and `bin/qt-cmake` are POSIX shell wrappers that hardcode the Qt build machine's prefix. Two variants exist in the same release: `arm64-v8a` is built on macOS (`/Users/qt/work/install`) and `x86_64` on Linux (`/home/qt/work/install`). Relocating only one leaves `bin/qmake` dead (`…/qmake6: not found`) while CMake still works, which is easy to miss. Setup needs both rebases plus `bin/target_qt.conf` set to `HostPrefix=../../gcc_64` and `HostSpec=linux-g++`; `tools/qt-android-kit.py <abi>` performs the download, extraction and relocation. The kit then reports `QT_VERSION=6.12.0`, `QMAKE_XSPEC=android-clang`, `QT_HOST_PREFIX=$HOME/Qt/6.12.0/gcc_64`.
- Host tools (`androiddeployqt`, `moc`, `qmake`) come from the desktop `gcc_64` kit, not from the Android kit.
- `androiddeployqt` invokes the Gradle wrapper; the first APK build downloads a Gradle distribution (~150 MB) from `services.gradle.org`.

**Host readiness (verified on this machine, 2026-11-07):**

- `~/Qt/6.12.0/gcc_64` — Qt 6.12.0 desktop kit.
- `~/Qt/6.12.0/android_arm64_v8a` — Qt 6.12.0 Android kit (relocated as described above).
- `~/Qt/Tools/OpenJDK-17` — Temurin 17.0.21.
- `~/Android/Sdk` — cmdline-tools `latest`, platform-tools r37.0.1, platforms `android-35` and `android-36`, build-tools 36.0.0.
- `~/Android/Sdk/ndk/27.2.12479018` — NDK r27c.
- A minimal QML app cross-compiles to `ELF 64-bit ARM aarch64 ... for Android 35, built by NDK r27c`.
- The same app packages end to end: `cmake --build build --target apk` produced a 63 MB debug APK, `arm64-v8a`, `minSdk 28`, `targetSdk 37`, debug-signed, via `androiddeployqt` + Gradle 9.5.1. Gradle is fetched by the wrapper at first build; point `distributionUrl` at a mirror if the default is slow.
- Not installed: Qt Charts, Qt WebSockets, Qt Multimedia, Qt 3D/Graphs, Qt SerialPort. WebSockets are not needed — the control plane is raw TCP, which `QTcpSocket` covers.
- `adb` is present under `~/Android/Sdk/platform-tools` but is not on `PATH`.

### 4.6 Deployment

- Desktop: `qt_generate_deploy_qml_app_script` + `install(SCRIPT ...)` runs the platform deploy tool at install time (`Qt6QmlMacros.cmake:4830-4870`; `Qt6CoreDeploySupport.cmake:650`). `NO_UNSUPPORTED_PLATFORM_ERROR` keeps Linux configure from failing where no deploy tool exists.
- Linux packaging — **one strategy, not two**: ship the self-contained tree produced by the deploy script (Qt libraries, QML imports and plugins are copied into the install prefix), wrapped in a CPack DEB with **no** Qt `Depends`. A DEB that depends on system `qt6-base`/`qt6-declarative` is the opposite approach and would either duplicate Qt or break at install; it is rejected because the apt Qt 6.10.2 cannot run the deploy script at all (see §4.5). AppImage via linuxdeploy + linuxdeploy-plugin-qt remains an alternative packaging of the same self-contained tree, not a different dependency model.
- Android: APK for local testing, AAB for distribution (per-ABI splits keep size sane). Release signing: set the CMake variables `QT_ANDROID_SIGN_APK` / `QT_ANDROID_SIGN_AAB` (checked at `Qt6AndroidMacros.cmake:717` and `:721`) together with `QT_ANDROID_KEYSTORE_PATH`, `QT_ANDROID_KEYSTORE_ALIAS`, `QT_ANDROID_KEYSTORE_STORE_PASS`, `QT_ANDROID_KEYSTORE_KEY_PASS` (defined in `QtAndroidSignPackage.cmake`).

### 4.7 `.gitignore` additions

The existing file ignores `build`, `build/`, `build_headless/`, `build_no_sqlite/` but no Qt build directories (`.gitignore:1-55`). Add:

```
build_qt/
build_qt_android/
android-build/
*.apk
*.aab
*.aar
```

---

## 5. TLS + token design for the control socket

### 5.1 Threat model and scope

Today the control socket is plaintext TCP with **no authentication, no authorization, no TLS, no IP allowlist and no connection rate limiting** (`src/control/JsonRpcServer.cpp:69-157`, `:312-360`; `docs/OPERATIONAL_GUIDE.md:308`: "anyone who can reach the port can place orders"). It binds `127.0.0.1` by default and validation never checks for loopback (`src/core/config.hpp:245-247`; `src/core/config_validator.cpp:389-411`).

The token layer closes the "any local process can place orders" half. TLS closes the "plaintext over the network" half that appears once `bindAddress` moves off loopback. Neither replaces a tunnel: a phone client on a LAN should still prefer Tailscale/WireGuard or an SSH tunnel, in which case TLS is optional and the token is a second factor.

Explicitly **not** in scope: per-client identity, token expiry/revocation lists, mTLS, challenge-response, audit logging of who issued which call.

### 5.2 Configuration surface

Extend `ControlConfig` (`src/core/config.hpp:243-257`) and `parseControl` (`src/core/config_loader.cpp:883-901`):

| Key | Type | Default | Meaning |
| --- | ---- | ------- | ------- |
| `tls_enabled` | bool | `false` | Wrap accepted sockets in `asio::ssl::stream`. |
| `tls_cert_path` | string | `""` | PEM certificate chain. Required when `tls_enabled`. |
| `tls_key_path` | string | `""` | PEM private key. Required when `tls_enabled`. |
| `tls_ca_path` | string | `""` | CA bundle used to verify **client** certificates when `tls_require_client_cert` is true. |
| `tls_require_client_cert` | bool | `false` | mTLS. Leave false in v1. |
| `token` | string | `""` | Shared bearer secret. Empty disables auth. Document `token = "from_env:PULSE_CONTROL_TOKEN"`. |

`from_env:` substitution already exists in the loader (`src/core/config_loader.cpp:34-53`; usage documented at `docs/OPERATIONAL_GUIDE.md:292`), so the token never has to live in `trading.toml`. An unset variable resolves to an empty string, i.e. auth stays off unless the operator opts in — the default-unchanged requirement holds automatically.

Client-only keys for `cli`/`mcp` (not engine-side): `--token`, `--tls`, `--ca`, `--server-name`, or the same TOML keys read by `loadControlConfig` (`apps/pulsetrader/main.cpp:1763-1793`). Precedence: CLI flag > env > TOML.

### 5.3 Server-side changes

| # | File / site | Change |
| - | ----------- | ------ |
| S-1 | `src/core/config.hpp:243-257` | Add the six fields to `ControlConfig`. |
| S-2 | `src/core/config_loader.cpp:893-896` | Parse them with `toml::find_or`, matching the existing mixed `bindAddress` / `display_timezone` naming. |
| S-3 | `src/core/config_validator.cpp:389-412` | Hard failures only: `tls_enabled` requires non-empty, readable cert and key paths → new error code 9105. Insecure combinations cannot be expressed as validator errors (it returns `PulseError` or success, with no warning channel), so log them at startup in `runTrade()`: `bindAddress` not loopback with TLS and token both off; `token` set with `tls_enabled=false`; `tls_enabled=true` with an empty token. |
| S-4 | `src/control/JsonRpcServer.hpp:45-46` | Keep the existing 3-argument constructor source-compatible (`apps/pulsetrader/main.cpp:1281-1285`, `tests/unit/control/test_control_client.cpp:34,50,85,107`). Add a defaulted 4th `Security` argument or a `setSecurity()` called before `start()`. |
| S-5 | `src/control/JsonRpcServer.cpp:176-207` | Build one `asio::ssl::context` in `start()` after `listen()` (line 186) and before spawning the accept thread (line 202). Failure → log, return false, code 9105. |
| S-6 | `src/control/JsonRpcServer.cpp:285-292` | Branch the accepted-socket path on `m_security.tlsEnabled`. |
| S-7 | `src/control/JsonRpcServer.cpp:312-347` | Extract the line loop into a template over `SyncReadStream`; add `handleSessionPlain` and `handleSessionTls` wrappers. TLS path: `stream.handshake(stream_base::server, ec)` → auth gate → unchanged loop. |
| S-8 | `src/control/JsonRpcServer.cpp:231-248` | `stop()` must close the **lowest layer** of each SSL stream. Keep the raw `shared_ptr<tcp::socket>` alongside so `shutdown()`/`close()` still wakes a blocked SSL read. |
| S-9 | `src/control/CMakeLists.txt:26-40` | Add `OpenSSL::SSL` and `OpenSSL::Crypto` PRIVATE. They already reach the binary transitively via `pulse::exchange`, but that coupling is accidental. |
| S-10 | `src/core/PulseError.hpp:100-105` | Add `ControlTlsInitFailed = 9105`, `ControlAuthFailed = 9106`. |

Implementation constraints:

- Standalone asio 1.30.2 with SSL support is installed; `ASIO_STANDALONE` is defined per translation unit in four files (`src/control/JsonRpcServer.cpp:4`, `src/control/ControlClient.cpp:4`, `src/exchange/GateWsClient.cpp:9`, `src/exchange/ProxyTunnel.cpp:4`). Include `<asio/ssl.hpp>` after that block; no CMake define and no Boost.
- `asio::ssl::stream<tcp::socket>` satisfies `SyncReadStream`/`SyncWriteStream`, so `read_until`/`write` keep working unchanged over TLS.
- The build uses `-Werror`, so OpenSSL 3 deprecations are build failures (`cmake/compiler_flags.cmake:19-24`). Use `SSL_CTX`/`asio::ssl::context`, `EVP_*`, `SHA256`, `CRYPTO_memcmp`; never `SSL_library_init`/`SSLv23_method`.
- Do not multiplex TLS and plaintext on one port with protocol sniffing. Pick one per configuration.

### 5.4 Authentication protocol

Auth is enforced in the **transport**, not in the method registry. Registry keys are the public control/MCP method list, and `dispatchLine` is a pure static function exercised by 12 unit tests (`tests/unit/control/test_json_rpc_server.cpp:35-143`); adding auth there would change its signature and break all 12. MCP forwards method names verbatim (`src/control/McpServer.cpp:723`).

Sequence per connection:

1. TCP accept.
2. If `tls_enabled`: TLS handshake with a 5 s budget.
3. If `token` is non-empty: read one line, require it to be a JSON-RPC request with `method == "auth"` and `params` an object containing a string `token`. Compare digests in constant time. Success → write `{"jsonrpc":"2.0","id":<echoed>,"result":{"authenticated":true}}`, continue. Failure or any other first method → write `-32603` with `data.code` 9106, message `authentication failed`, then close. Cap failed attempts per connection at 1.
4. If `token` is empty: skip step 3 entirely and enter the normal loop. The plaintext, no-auth path must remain byte-identical.
5. Normal loop, unchanged.

Token comparison:

- At `start()`, store `SHA-256(expected_token)` as a fixed 32-byte array. Reject an empty configured token before this point so an empty presented token can never match.
- Per attempt, compute `SHA-256(presented)` and compare with `CRYPTO_memcmp` over exactly 32 bytes (`/usr/include/openssl/crypto.h:470`; `sha.h:82`). Digesting first hides the token length, which a raw `memcmp` over `std::string` cannot.

Client rules:

- Send `auth` immediately after connect (and TLS handshake) whenever a token is configured, before any other request.
- If the server answers `-32601` / 9102 to `auth`, the server has auth disabled. Default behavior: fail with "engine does not support token authentication" (a security downgrade must be explicit). A client setting may allow it, but the default is fail.
- If the client has no token and the server requires one, the first real method returns `-32603` / 9106 and the connection closes; surface as "authentication required", not as "engine unreachable".

`auth` is a new reserved method name. It must not be added to `makeMethodRegistry`, and it must not be reused by a future engine method (risk R-10).

### 5.5 Client-side TLS

- `QSslSocket` is used when TLS is enabled; `QTcpSocket` otherwise. Both derive from `QAbstractSocket`, so `RpcClient` holds a `QAbstractSocket*` and the framing code is shared.
- Before `connectToHostEncrypted`, set `QSslConfiguration`: protocol TLS 1.2+, `PeerVerifyMode::VerifyPeer`, system CA certificates or a bundled CA, and the expected server name. `connectToHostEncrypted(host, port, expectedServerName)` supplies SNI.
- Never call `ignoreSslErrors()`.
- Android 7+ apps do not trust user-installed CAs by default. Two viable routes: (a) a publicly trusted certificate on a real hostname; (b) a private CA bundled in the app resources and added with `QSslConfiguration::addCaCertificate`. Pick one before M-Q6.
- `cli`/`mcp` cannot reuse `bindAddress` as the verification name: it is usually `127.0.0.1`, which never matches a certificate SAN (`apps/pulsetrader/main.cpp:1859`, `:1892`; `src/core/config.hpp:246`). Add the separate `--server-name` / `tls_server_name` client key.

### 5.6 Compatibility matrix

| Row | Server | Client | Wire | Safe on untrusted network? | Notes |
| --- | ------ | ------ | ---- | -------------------------- | ----- |
| 1 | plaintext, no token (today) | plaintext, no token | Unchanged; existing tests must pass untouched | No (loopback only) | Default. Any behavior change here is a design bug. |
| 2 | TLS + token | TLS + token | TLS 1.2+, auth exchange, then normal lines | **Yes** | The only row intended for remote/mobile use. |
| 3 | TLS, no token | TLS, no token | Encryption only | Confidentiality yes, access control no | Startup WARN. Any process that reaches the port can place orders. |
| 4 | plaintext + token | plaintext + token | Bearer secret in clear | Only over an encrypted tunnel (WireGuard/Tailscale/SSH) | Cheapest to ship; defends against other local users, not an on-path attacker. |

Reconnect must re-run the whole sequence, because `ControlClient::call` re-invokes `connect()` on a stale socket (`src/control/ControlClient.cpp:106-134`). Qt has its own reconnect loop and must do the same: TLS handshake + auth on every reconnection.

### 5.7 Failure modes and codes

| Condition | Wire result | Client behavior |
| --------- | ----------- | --------------- |
| TLS configured but cert/key unreadable | Engine fails `start()`, logs, code 9105 | Engine runs without remote control; client sees connection refused |
| TLS handshake fails (untrusted cert, protocol mismatch) | Socket closed, no protocol frame | Distinguish "certificate rejected" from "engine unreachable"; never auto-retry a certificate failure in a loop |
| Wrong/missing token | `-32603` + `data.code` 9106, then close | Auth dialog; do not hammer reconnect |
| Auth method on a no-auth server | `-32601` + 9102 | "Engine does not support token auth" (default fail) |
| Client connects plaintext to a TLS server | Handshake bytes are not JSON → server answers `-32700` and keeps reading, or closes | Timed out request; fail fast within the handshake budget, do not hang |
| Token empty + non-loopback bind | Engine starts with a WARN | UI shows an "insecure deployment" banner when connecting without TLS |

### 5.8 Resource hardening shipped with this change

These are pre-existing defects that become materially worse once the port is reachable off-host. Fix them in the same transport change.

| # | Issue | Fix |
| - | ----- | --- |
| H-1 | Unbounded line buffer (`src/control/JsonRpcServer.cpp:316`) | `asio::streambuf buffer{64 * 1024}`; treat `length_error` or `error::not_found` from a full buffer as a protocol violation and close. |
| H-2 | Unlimited concurrent connections (`:287-292`) | Hard cap `kMaxSessions = 8`; beyond it, accept and immediately close. |
| H-3 | No handshake/auth/idle deadline (`:312-347`) | Set `SO_RCVTIMEO` to 5 s on the accepted socket for the TLS+auth phase, then clear it. Optionally add a 15 min idle timeout for authenticated sessions. |
| H-4 | Finished threads never reaped (`:292-302`) | Replace `std::vector<std::thread>` with detached threads plus an `std::atomic<int>` active count; `stop()` closes sockets and waits (bounded, e.g. 2 s) for the count to reach 0. |

Do not change `dispatchLine` semantics while doing this: all 12 dispatch tests and all 5 loopback client tests must pass unmodified.

### 5.9 Certificate and key handling

- Keep cert and key under `data/` (gitignored) or outside the repository; never commit them (`AGENTS.md`, Git section).
- Refuse to start if the key file is group- or other-readable. This repo has a documented history of world-readable secret files (`.env` at mode 664).
- A self-signed leaf with a long expiry is the realistic first iteration, but document rotation at the same time: token rotation and cert rotation both mean "restart the engine" in v1.

---

## 6. Client architecture

### 6.1 Layering

```
QML (declarative UI, no I/O)
   │  properties + invokables + models
EngineController (QObject: state machine, poll timers, mapping)
   │
RpcClient × 3 (transport: framing, correlation, timeouts, reconnect)
   │
QTcpSocket / QSslSocket
   │
newline-delimited JSON-RPC 2.0 → engine L9
```

The QML layer never creates a socket and never parses JSON. The C++ layer never formats money or timestamps for display beyond what the engine already provides.

### 6.2 `RpcClient`

```cpp
class RpcClient : public QObject
{
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Authenticating, Ready, Failed };

    struct Endpoint {
        QString host;
        quint16 port = 8081;
        bool    tls = false;
        QString serverName;      // SNI + verification name
        QString token;           // empty = no auth step
        QList<QSslCertificate> caCertificates;
    };

    void connectToEngine(const Endpoint &endpoint);
    void disconnectFromEngine();
    [[nodiscard]] quint64 call(const QString &method,
                               const QJsonObject &params = {},
                               int timeoutMs = 10000);
    [[nodiscard]] State state() const;

signals:
    void stateChanged(State state);
    void resultReceived(quint64 id, const QJsonObject &result);
    void callFailed(quint64 id, const RpcError &error);
    void transportError(const RpcError &error);
};
```

Behavior:

1. `call()` allocates a monotonically increasing `quint64` id, stores `{id → {method, QTimer, startedAt}}` in a `QHash`, writes `QJsonDocument(request).toJson(Compact) + '\n'`, and returns immediately. Never block, never `waitForReadyRead`.
2. On `readyRead`, append to a `QByteArray` buffer, split on `\n`, strip trailing `\r`, ignore empty lines, parse each line with `QJsonDocument`.
3. Correlate by `id`. A reply whose id is unknown and not in the recently-expired set is a desync → disconnect and reconnect.
4. Enforce the response cap: if the buffer exceeds 1 MiB without a newline, or a single line exceeds 1 MiB, raise a protocol error and reconnect. Cap outgoing requests at 64 KiB.
5. Per-request `QTimer`; on expiry emit `callFailed(id, Timeout)` and remember the id as recently-expired (bounded set, e.g. 64 ids) so a late reply is discarded rather than misattributed.
6. Reconnect: TCP connect (or TLS handshake) → auth step if a token is configured → `Ready`. Emit `stateChanged` at each transition.
7. Log method, id, elapsed ms and outcome. **Never** log `params` for `auth`; never log the token.

`RpcError` taxonomy:

```cpp
enum class Kind { Transport, TlsHandshake, Timeout, AuthFailed, RpcError, InBand, ProtocolDesync };
struct RpcError {
    Kind kind;
    QString message;        // engine-provided text when available
    int jsonRpcCode = 0;    // -32700, -32600, -32601, -32603
    int dataCode = 0;       // PulseError value from error.data.code
};
```

### 6.3 `EngineController`

- Owns three `RpcClient` instances — `fast`, `rest`, `slow` — created once and kept for the session (Section 6.6). All three use the same endpoint and credentials.
- Owns one single-shot `QTimer` per polled method, plus an `inFlight` flag per method so a slow reply never stacks requests.
- Exposes `Q_PROPERTY`s: `connectionState`, `engineVersion`, `engineUptimeSec`, `network`, `tradingHalted`, `activeMarket`, `symbols`, `lastError`, `accountState`.
- Exposes `Q_INVOKABLE void connectToEngine(const QVariantMap &profile)`, `disconnectFromEngine()`, `refreshAll()`, `setPollingEnabled(bool)`.
- Exposes the models as `Q_PROPERTY(QAbstractItemModel *positionsModel READ positionsModel CONSTANT)` and equivalents.
- Applies the wire→UI mapping rules: strings to enums, unknown strings to `Unknown`, absence to `Unknown`/`Absent` (never `0`), `*_str` passed through verbatim, age computed from raw epoch fields against `QDateTime::currentMSecsSinceEpoch()`.
- On `applicationStateChanged` (Android) or window minimized (desktop), stops all timers; on resume, performs one immediate full refresh and restarts the schedule.
- **QML registration (required, and not done by `main.cpp`).** `EngineController` carries `QML_ELEMENT` and is listed under `qt_add_qml_module(... SOURCES ...)`; `Main.qml` then instantiates it (`EngineController { id: engine }`) and passes it to pages. The five models are `Q_PROPERTY`s on that object, so they need no separate registration. Verified: adding `QML_ELEMENT` to a class listed under `SOURCES` on an executable-backed module produces `exports: ["Pulse.Client/EngineController 1.0"]` in the generated qmltypes. Without this step the pages have no data source and M-Q3's acceptance criteria cannot be met.

### 6.4 Models

| Model | Rows | Identity | Update strategy |
| ----- | ---- | -------- | --------------- |
| `PositionsModel` | `result.positions` | `position_id` | Keyed merge; `beginResetModel` only on symbol-set change. Roles: all 19 fields + `ageMs`, `isStale`, `sideEnum`, `marketEnum`. |
| `OrdersModel` | `result.activeOrders` | `order_id` | Keyed merge; preserve selection. |
| `ReportsModel` | `result.recentReports` | `(order_id, fill_time)` | Replace list; preserve scroll position. Roles include a precomputed `elapsedMs` from `fill_time / 1e6`. |
| `SignalsModel` | `result.signals` | `source` | Keyed merge; `ageMs` from `ts_ms`; `isStale = ageMs > 120000`. |
| `AccountModel` | — | — | Not a list: `Q_PROPERTY` groups `futuresPresent`, `futuresTotal`, `spotPresent`, `spotAvailable`, `cfdPresent`, ... with explicit presence flags. |

Rules:

- Never render a raw `QJsonObject` in a delegate; extract typed roles in `data()`.
- Never let QML do arithmetic on money fields; sums come from the engine's `portfolio` block.
- Sorting and filtering live in `QSortFilterProxyModel`, not in QML.
- A position disappearing from the response must be removed from the model, not left as a ghost row.

### 6.5 QML layer

- `Main.qml`: `ApplicationWindow` with an adaptive shell — a permanent side rail at width ≥ 900 dp, a bottom `NavigationBar`/`Drawer` below that. Android uses large touch targets (≥48 dp).
- Pages: Dashboard (status tiles, connection state, halt badge, account summary), Positions, Orders (active + recent tabs), Signals (cards with confidence, age, stale badge), Market (symbol picker from `get_status.symbols` plus free text, ticker, depth, kline table or sparkline), Settings (endpoint, TLS, token, poll intervals, theme).
- `Style.qml` singleton holds colors, spacing, typography and dark/light tokens. Do not hard-code colors in pages.
- `StatusBar` is always visible: connection state, engine version, uptime, `trading_halted` badge, active market.
- `StaleBadge` appears when a section's last successful refresh is older than 2× its poll interval.
- `ErrorBanner` shows transport errors without hiding the last known data; the data keeps its stale badge.
- Every page has an `EmptyState` for "no positions", "no orders", "symbols empty".
- No business logic in QML; expressions are limited to bindings and display strings.

`main.cpp`:

```cpp
int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("pulseTrader"));
    QGuiApplication::setOrganizationName(QStringLiteral("pulseTrader"));
    QQuickStyle::setStyle(QStringLiteral("Material"));   // before QML loads

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Pulse.Client", "Main");
    return app.exec();
}
```

### 6.6 Threading model

- **All I/O stays on the GUI thread.** `QTcpSocket`/`QSslSocket` are event-driven; every request is asynchronous. No `QThread` in v1.
- Justification: the largest payload is `get_orders` (≤60 reports, tens of KB); parsing is microseconds.
- **Three connections, not one.** The server dispatches one request at a time per connection (`src/control/JsonRpcServer.cpp:317-346`), so a slow `get_account` (up to ~120 s worst case) or a REST-backed `get_positions` would stall `get_status` if they shared a socket.
  - `fast`: `get_status`, `get_signals`, `get_market` — all in-memory, sub-millisecond handlers.
  - `rest`: `get_positions`, `get_orders` — may block on exchange REST.
  - `slow`: `get_account` — may block on the global REST mutex for minutes.
- Three long-lived sockets also avoid the server's thread-accounting leak (`src/control/JsonRpcServer.cpp:292-302`); reconnect churn is the thing to minimize.
- If a future method returns megabyte payloads, move that `RpcClient` into a worker `QThread` with queued connections; the API above already fits that change.

### 6.7 Reconnect and degradation

State machine: `Disconnected → Connecting → (TlsHandshake) → Authenticating → Ready`, plus `Degraded` (connected but stale) and `Failed` (fatal, e.g. certificate rejected).

| Trigger | Action |
| ------- | ------ |
| Connect refused / DNS failure | Backoff retry: 0.5, 1, 2, 4, 8, 15, 30 s (cap) with ±20% jitter; reset on success. |
| Socket closed unexpectedly | Immediate single retry, then backoff. |
| Request timeout | Mark that section stale; do not close the socket unless two consecutive `fast` requests time out. |
| Response desync (unknown id, oversized line) | Close and reconnect immediately once; then backoff. |
| TLS certificate rejection | **Stop.** Show a certificate dialog; no automatic retry loop. |
| Auth failure (9106) | **Stop.** Show a credentials dialog; no automatic retry loop. |
| Engine restart | Detected by transport error; reconnect re-runs TLS + auth, then a full refresh. |

Never reconnect in a tight loop: each attempt costs the engine a thread that is never reaped.

### 6.8 Polling schedule

The table in Section 3.7 is normative. Additional rules:

- Only poll what the visible page needs; `MarketPage` polls `get_market` only for its selected symbol(s).
- `get_market` is polled per symbol, not for all `symbols` at once.
- `account` refresh is independent of page visibility (it is slow and users expect it warm), but at 30 s it is cheap.
- A failed request does not change the interval; the next tick tries again.
- On `Ready`, fire all enabled polls immediately in dependency order: `get_status` first (it supplies `symbols` for the market page), then the rest.

### 6.9 Error taxonomy → UI

| Source | Detection | UI |
| ------ | --------- | -- |
| Transport | `RpcError::Kind::Transport` | Status bar "engine unreachable"; keep last data with stale badge |
| Timeout | `Kind::Timeout` | Section stale badge + banner "no response from engine" |
| TLS | `Kind::TlsHandshake` | Dialog with certificate details; no auto-retry |
| Auth | `Kind::AuthFailed` | Credentials dialog; no auto-retry |
| JSON-RPC `-32601` / 9102 | `Kind::RpcError`, `jsonRpcCode == -32601` | "Engine version does not support <method>" |
| JSON-RPC `-32603` / 9101 | client bug or bad params | Show engine message; log |
| JSON-RPC `-32603` / other | engine-side failure | Show `message`; classify only known codes |
| In-band `result.error` | `Kind::InBand` | Inline in the market widget; not a banner |
| `available:false` | semantic absence | "unknown", never `0.00` |

### 6.10 Client-side secrets

- Token persistence is **opt-in** ("remember token"). If enabled, store in `QSettings` (Android: SharedPreferences) and show a one-time warning that it is not encrypted at rest. v1 does not integrate an OS keychain.
- Tokens are never written to logs, crash reports or the QML layer.
- CA bundles for a private CA are compiled into the app as resources.
- The client refuses `ignoreSslErrors()` and refuses to downgrade from TLS when the server advertises plaintext.

---

## 7. Milestones

Ordered by dependency. Each milestone is mergeable on its own and leaves the engine's default behavior unchanged.

### M-Q1 — Standalone skeleton and build isolation

**Depends on:** nothing.

**Deliverables**

- `client/qt/` tree with `CMakeLists.txt`, `main.cpp`, `Style.qml`, an empty `Main.qml`.
- Root `PULSE_BUILD_QT_CLIENT` option (default OFF) and guarded `add_subdirectory`.
- `.gitignore` additions.
- `CMakePresets.json` for linux-debug/linux-release.

**Acceptance criteria**

- `cmake -S client/qt -B build_qt -GNinja -DCMAKE_PREFIX_PATH=$HOME/Qt/6.12.0/gcc_64 && cmake --build build_qt` succeeds on Ubuntu with Qt 6.12.0.
- `QT_QPA_PLATFORM=offscreen ./build_qt/pulseTraderQt` starts and exits cleanly (CI smoke).
- Root `cmake -B build -S .` produces the same configuration as before the change; `-DPULSE_BUILD_QT_CLIENT=ON` adds the client.
- `git status` shows no build artifacts; no Qt entry in `vcpkg.json` or the root `CMakeLists.txt` component list.
- Windows and macOS configure+build pass on hosts with the corresponding Qt kits (recorded in the PR; this Linux host cannot validate them).

### M-Q2 — `RpcClient` and protocol conformance

**Depends on:** M-Q1.

**Deliverables**

- `RpcClient`, `RpcError`, `RpcMethods`.
- Fake-engine `QTcpServer` fixture in `tests/tst_rpcclient.cpp`.

**Acceptance criteria**

- Framing tests: two requests in one TCP segment; one response split across two segments; CRLF; an empty line; a connection closed mid-line.
- Error mapping tests: `-32700`, `-32600`, `-32601`/9102, `-32603`/9101, `-32603`/9103 all map to the right `RpcError`.
- Correlation tests: out-of-order reply is matched by id; unknown id triggers desync handling; late reply after timeout is discarded.
- Caps: a 1 MiB+ line closes the connection with a protocol error; a 64 KiB request is refused client-side.
- Timeout: a silent server produces `Kind::Timeout` at the configured deadline.
- Reconnect: kill the fake server, restart it, and the client returns to `Ready` within 30 s.
- Manual integration: against a live engine on `127.0.0.1:8081`, all six methods return parseable results.

### M-Q3 — Read-only monitoring UI

**Depends on:** M-Q2.

**Deliverables**

- `EngineController` with three `RpcClient`s and poll timers.
- All five models.
- Dashboard, Positions, Orders, Signals, Market, Settings pages; `StatusBar`, `StaleBadge`, `ErrorBanner`, `EmptyState`.

**Acceptance criteria**

- Each of the six methods is rendered with every documented field available somewhere in the UI (some as tooltips/details).
- `get_account` with `available:false` shows "unknown"; absent `spot`/`cfd` blocks show "not configured/failed", never `0.00`.
- `get_market` in-band `result.error` renders inline and leaves the rest of the page usable.
- Signals older than 120 s carry a visible stale badge; the aggregate `null` case renders "no consensus yet".
- All timestamps shown come from `*_str`; no client-side reformatting of the engine's timezone.
- The UI stays responsive (animations continue, status bar keeps updating) while `get_account` is pending on the slow connection.
- Layout at 360 dp width has no horizontal scrolling and no clipped labels.
- Model tests cover keyed merge (position removed, order filled, signal disappeared) and unknown enum strings.

### M-Q4 — TLS + token, end to end

**Depends on:** M-Q2 (client transport), M-Q1 (engine build). Engine work can proceed in parallel with M-Q3.

**Deliverables**

- Engine: S-1..S-10 from Section 5.3; `cli`/`mcp` flags; docs (OPERATIONAL_GUIDE, trading.toml comments).
- Client: TLS mode, token entry, CA bundle support, auth step, distinct failure dialogs.

**Acceptance criteria**

- Matrix row 1: all existing engine tests pass unmodified — 12 `dispatchLine` tests (`tests/unit/control/test_json_rpc_server.cpp:35-143`) and 5 loopback client tests (`tests/unit/control/test_control_client.cpp:32-115`).
- Row 2: TLS + token works from a second machine (or a second network namespace) against a non-loopback bind.
- Row 3: TLS without token connects; engine logs a WARN.
- Row 4: token without TLS connects and is rejected with 9106 when wrong.
- Wrong token surfaces as "authentication failed", never as "unreachable"; the client stops retrying.
- Plaintext client against a TLS server fails within the handshake budget (no indefinite hang).
- An untrusted certificate is rejected and the request is not sent.
- Cert/key permission check rejects a world-readable key file at startup.

### M-Q5 — Hardening and degraded-network validation

**Depends on:** M-Q4 for the transport rewrite; can be developed alongside.

**Deliverables**

- H-1..H-4 from Section 5.8.
- Client degradation behavior from Section 6.7.
- A soak test script.

**Acceptance criteria**

- A line longer than 64 KiB is closed without exhausting engine memory.
- The 9th concurrent connection is closed immediately; the first 8 keep working.
- After 100 connect/disconnect cycles, the engine's thread count and file-descriptor count are flat.
- A client idle for 30 minutes still works or is cleanly reaped (depending on the idle-timeout decision).
- Client soak: 2 hours of polling with an engine restart in the middle; no memory growth, no duplicate rows, no frozen UI during the outage.
- `get_account` timing out does not disturb the fast connection's 1 s status cadence.

### M-Q6 — Packaging and Android

**Depends on:** M-Q3 for a usable app; M-Q4 for the remote story.

**Deliverables**

- Linux: DEB (or AppImage) with desktop file and icon.
- Windows: deployed directory + Inno Setup installer.
- macOS: signed `.app` + `.dmg`.
- Android: Qt-for-Android kit install, `android/` sources, APK and AAB targets, INTERNET permission, CA strategy.

**Acceptance criteria**

- Each desktop artifact launches on a clean machine (or VM without Qt installed) and connects to an engine.
- macOS build is universal (`arm64;x86_64`) and signed/notarized for distribution.
- APK installs and runs on API 28 and API 36 (device or emulator); AAB builds.
- The Android app connects over TLS + token; the chosen CA strategy is documented in the release notes.
- App size per ABI is recorded; AAB per-ABI splitting confirmed.
- CI (or a documented manual script) reproduces every artifact from a clean checkout.

---

## 8. Risks and open questions

### 8.1 Risks

| ID | Risk | Impact | Mitigation |
| -- | ---- | ------ | ---------- |
| R-1 | `get_account` can block for ~40 s per REST client (three clients serialised behind one mutex) and reports failure only by omitting keys. | UI freeze if polled naively; users misread missing data as zero. | Dedicated slow connection; 120 s timeout; explicit "unknown" rendering; 30 s polling. |
| R-2 | Server thread-per-connection bookkeeping never shrinks (`src/control/JsonRpcServer.cpp:292-302`). | A GUI that reconnects often grows engine memory for the process lifetime. | Three long-lived sockets; backoff capped at 30 s; fix in M-Q5 (H-4). |
| R-3 | No server push. | Polling load and visible latency; `get_positions`/`get_orders` trigger exchange REST calls every poll. | Conservative intervals (Section 3.7); poll only visible pages; one client instance per user. |
| R-4 | Ticker staleness is not signalled by `get_market`. | The market page can show an old price as current. | Age from `ticker.timestamp`; stale badge past 5 s; do not use the price for decisions. |
| R-5 | Timestamp units differ between `activeOrders` (ms) and `recentReports` (ns) in one response. | Wrong times if the raw fields are formatted naively. | Render `*_str` only; derive age with an explicit unit conversion; unit tests. |
| R-6 | Android does not trust user-installed CAs by default. | TLS fails against a private CA. | Public CA or bundled private CA with `addCaCertificate`; decide before M-Q6. |
| R-7 | ~~Android toolchain missing on the current host~~ **Resolved 2026-11-07**: the Qt 6.12 Android kit, JDK 17, Android SDK (API 35 and 36) and NDK r27c are installed, and a QML app cross-compiles to an ARM64 Android binary. Remaining constraint: NDK r27c caps at API 35, so a targetSdk of 36 requires NDK r28+. | M-Q6 still needs a device/emulator run. | Re-run the §4.5 setup on any other build machine; the relocation steps are mandatory there too. |
| R-8 | Qt Charts is absent. | No candlestick chart in v1. | QML `Canvas` sparkline; add Qt Charts to every platform kit later if needed. |
| R-9 | `docs/architecture.md` still describes Layer 9 as the removed WebUI (`docs/architecture.md:386`), while `AGENTS.md:15` and `src/control/` define it as the JSON-RPC control plane. | Design docs disagree with the code; new contributors misread the layer. | Correct the Layer 9 section in the same change (repo rule: architecture.md is the source of truth). |
| R-10 | `auth` is a new method name; a future engine method could collide. | Silent protocol break. | Reserve it in `JsonRpcServer`/`McpServer` docs and in `AGENTS.md`; never add it to the registry. |
| R-11 | Single shared static token: no per-client identity, no expiry, no revocation; rotation requires an engine restart. | Weak auditability; a leaked token is valid until restart. | Document; keep TLS mandatory off-loopback; plan per-client tokens as a later engine change. |
| R-12 | If the client is added in-tree, it inherits `-Werror -Wall -Wextra -Wpedantic`. | Qt-generated code or third-party headers can break the build. | Keep the standalone configure as the primary mode; only enable the in-tree hook deliberately. |
| R-13 | No capability negotiation; an older engine returns `-32601` for missing methods. | Confusing partial UI. | Show a per-section "not supported by this engine" state on 9102. |
| R-14 | Windows/macOS builds and deployment cannot be validated on this Linux host. | Packaged artifacts may fail on first run. | Validate on real hosts before release; keep packaging scripts in the repo and reproducible. |
| R-15 | M-Q6 requires signing material that does not exist in this repository: an Apple Developer ID identity plus notarization credentials for the `.app`/`.dmg`, and an Android release keystore (`QT_ANDROID_KEYSTORE_PATH` / `_ALIAS` / `_STORE_PASS` / `_KEY_PASS`). | M-Q6 cannot close without them; an unsigned AAB cannot ship. | Obtain an Apple Developer account and generate a keystore before M-Q6 starts; never commit either. |
| R-16 | `get_positions` and `get_orders` issue one Gate REST call per tracked order per poll, each capable of ~40 s of retry. | The 15 s / 30 s client timeouts fire while the engine is still working, producing spurious errors. | Treat a timeout as "engine busy" rather than failure; raise the timeouts or move these two methods to the slow socket with a 90 s budget before M-Q3. |

### 8.2 Open questions

| ID | Question | Blocks |
| -- | -------- | ------ |
| Q-1 | Is 64 KiB request / 1 MiB response the right frame cap? Nothing in the engine answers this; the server's effective bound is `SIZE_MAX`. | M-Q2 |
| Q-2 | Should the engine refuse to start (not merely warn) when `bindAddress` is not loopback and neither TLS nor a token is configured? The current default-preserving constraint allows only a warning. | M-Q4 |
| Q-3 | One shared token, or per-client named tokens with revocation? Per-client tokens need a list and a reload path. | M-Q4 |
| Q-4 | Do any existing scripts, agents or sub-agent relays speak raw JSON-RPC to `127.0.0.1:8081` today? Any of them break when a token becomes mandatory, and not all are in this repository. | M-Q4 |
| Q-5 | Is mTLS in scope for a later release? It would replace the token but makes Android key distribution significantly harder. | Later |
| Q-6 | App identity values: bundle id `com.pulsetrader.client`, QML URI `Pulse.Client`, display name `pulseTrader`, icon assets. None exist yet. | M-Q1 |
| Q-7 | Should the Qt client be buildable from the root tree at all, given the root requires vcpkg/apt dependencies and `-Werror`? The default-OFF hook is cheap, but standalone configure may be the only mode actually used. | M-Q1 |
| Q-8 | `client_order_id` is absent from `activeOrders` (`src/core/snapshot_types.hpp:185-201`) although the struct carries it. Is that intentional? Without it, no grid/order-correlation UI is possible. | M-Q3 (feature scope) |
| Q-9 | Which authenticated method should the client use to distinguish "engine older than the client" from a transient error? Today only `-32601`/9102 is available. | M-Q3 |
| Q-10 | Does the Qt for Android default `AndroidManifest.xml` already include `android.permission.INTERNET`? Unverifiable on this host (no Qt for Android install). | M-Q6 |
| Q-11 | Which Android `targetSdk` to declare? `minSdk` is fixed at 28 by `QT_ANDROID_MIN_SDK_VERSION`; `targetSdk` is a separate `QT_ANDROID_TARGET_SDK_VERSION` property (`Qt6AndroidMacros.cmake:437`) but the current Play Store requirement for new listings was not verified, and NDK r27c cannot compile above API 35. | M-Q6 |
| Q-12 | Is `GET /api/v4/futures/usdt/accounts` an object or a single-element array? `GateRestClient.cpp:621-628` calls `j.value("total", ...)` directly with no `[0]` unwrap, unlike the spot path. If it returns an array, `get_account` throws and returns `-32603`/9103 rather than a balance. No fixture pins this. | get_account correctness |
| Q-13 | Are the CFD field names (`equity`, `margin_free`, `margin`, `unrealized_pnl`) and the `data` wrapper in `EngineServices.cpp:249-256` correct against the live tradfi endpoint? No recorded sample exists. | Account display |
| Q-14 | `Position::quantity` semantics are ambiguous in-repo: `src/core/types.hpp:50-54` documents base currency, while `PositionManager.cpp:196/389/429-430` multiplies by `quanto_multiplier` (contract semantics). Which does the UI label? | Positions display |
| Q-15 | Does Android's cleartext-traffic policy affect raw `QTcpSocket` connections? Not verified on-device. | Plaintext-on-LAN fallback |
| Q-16 | Should `switch_direction`'s in-band `result.error` pattern be treated as a general convention? It affects only future methods, but the client's result-handling helper should be designed for it now. | M-Q2 API shape |

### 8.3 Conflicts and corrections found while verifying the source material

1. **`recentReports` key count.** The supplied contract notes describe a report element as having "16 keys" but enumerate 15 fields plus two `*_str` companions. The source is authoritative: `ExecutionReport::to_json` emits 15 keys (`src/execution/ExecutionReport.cpp:10-38`) and `EngineServices::orders()` adds `submit_time_str` and `fill_time_str` (`src/control/EngineServices.cpp:347-354`), so an element has **17 keys**. The enumerated list in the notes is correct; only the count is wrong.
2. **`ControlClient` read buffer.** The header declares `std::string m_lineBuffer` (`src/control/ControlClient.hpp:60`), but `tryCallOnce` uses a local `asio::streambuf` per call (`src/control/ControlClient.cpp:156-164`). The field is dead, and the behavioral claim in the notes (no pipelining, no id correlation) is correct.
3. **`get_status.version` origin.** The notes say the value is a hardcoded literal `"0.1.0"` inside the engine. It is in fact passed as a constructor argument from `apps/pulsetrader/main.cpp:1247` into `EngineServices` (`src/control/EngineServices.cpp:112-135`), which stores it as `m_version`. Behaviorally identical, but the citation is the call site, not the service.
4. **`-32602`.** Confirmed never emitted by the TCP control socket (`grep -c 32602 src/control/JsonRpcServer.cpp` = 0); MCP uses it for malformed tool calls only (`src/control/McpServer.cpp:693-721`).

---

## Appendix A — Verified reference points

Files and lines used throughout this document, for a reviewer's quick pass:

- Framing and dispatch: `src/control/JsonRpcServer.cpp:69-160`, `:176-207`, `:225-305`, `:312-360`.
- Method registry: `src/control/JsonRpcServer.cpp:366-588`.
- Service builders: `src/control/EngineServices.cpp:178-207` (status), `:209-261` (account), `:263-293` (positions), `:295-355` (orders), `:362-384` (signals), `:507-560` (market).
- Serializers: `src/core/snapshot_types.hpp:43-138`, `:160-201`; `src/execution/ExecutionReport.cpp:10-38`.
- Signal board: `src/strategy/signal/SignalBoard.cpp:27-90`; `src/strategy/signal/SignalBoard.hpp:59` (std::map), `:30-34` (Entry); iteration order at `SignalBoard.cpp:75-78`.
- Config: `src/core/config.hpp:243-257`; `src/core/config_loader.cpp:883-901`; `src/core/config_validator.cpp:389-412`.
- Errors: `src/core/PulseError.hpp:98-121`.
- Reference client: `src/control/ControlClient.cpp:24-181`; `src/control/ControlClient.hpp:33-64`.
- Build: root `CMakeLists.txt:26-28`, `:38-59`, `:68-79`; `cmake/compiler_flags.cmake:7-24`; `apps/CMakeLists.txt:1-2`; `.gitignore:1-55`.
- Time formatting: `src/core/TimeUtil.hpp:136-195`.
- Stale Layer 9 description: `docs/architecture.md:386`.
