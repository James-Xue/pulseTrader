# Gate.io CFD (TradFi) — API Survey & Integration Plan

> Status: **researched & verified live** (2026-08-14) — implementation pending.
>
> This document captures the complete API survey of Gate.io's traditional-finance
> CFD product (gold, forex, indices) and the plan to extend pulseTrader with a
> third market type (`Cfd`). Product direction moved here from BTC_USDT perpetual
> futures on 2026-08-14.

---

## 1. What is Gate TradFi / CFD

Gate.io's CFD product ("TradFi" in the API) offers **traditional financial
instruments** — metals (gold `XAUUSD`, silver `XAGUSD`), forex pairs, indices —
traded through a **MetaTrader 5 (MT5) account** (`mt5_uid`). It is a separate
product line from spot / USDT perpetual futures, with:

- Its own **API namespace**: `/api/v4/tradfi/*`
- Its own **account and balance** (settled in **USD**; deposits/withdrawals in
  **USDT** only via a dedicated transfer endpoint)
- Its own **API-key permission** ("CFD" on the key management page — distinct
  from the "futures" permission)
- **No WebSocket** market-data channel (tickers and klines are REST-only)
- Broker-style order semantics: `volume` in **lots** (e.g. 0.01 lot = 1 oz for
  XAUUSD), side `1 = sell / 2 = buy`, optional take-profit / stop-loss attached
  to the order

### 1.1 Verified account state (2026-08-14, main account)

| Item | Value |
|---|---|
| CFD account | registered, `mt5_uid = 2017864` |
| Balance / equity | **81.86 USD** (margin 0, unrealized PnL 0) |
| Stop-out level | 50% |
| XAUUSD (Gold) | listed, `is_base: true`, status `open`, category Metals, settlement USD |
| XAUUSD price | ~4348 USD/oz (bid 4348.28 / ask 4348.38, 2026-08-14) |
| Contract spec | 1 lot = **100 oz**; min volume **0.01** lot; step 0.01; max 15 lots |
| Leverage options | 20 / 50 / 100 / 200 / 500 |

Notional math: `0.01 lot × 100 oz × 4348 = 4,348 USD`; margin at 500× ≈ **8.7 USD**
per 0.01-lot position → the current 81.86 USD balance supports ~9 × 0.01-lot
positions before margin limits.

---

## 2. API Reference (Gate API v4, `/api/v4/tradfi`)

All endpoints use the standard Gate v4 HMAC-SHA512 signing already implemented
in `gate_auth.hpp`. Public market-data calls work without a key; everything else
requires the **CFD permission** on the API key.

### 2.1 Symbols & market data (public except detail)

| Method | Endpoint | Notes |
|---|---|---|
| GET | `/tradfi/symbols` | Symbol list (categories, trade mode, session open/close times) |
| GET | `/tradfi/symbols/{symbol}/tickers` | Ticker: last/bid/ask/high/low/change, market status |
| GET | `/tradfi/symbols/{symbol}/klines` | Klines; `kline_type` e.g. `1m`; `limit` ≤ 500; times in Unix seconds |
| GET | `/tradfi/symbols/categories` | Trading categories |
| GET | `/tradfi/symbols/detail?symbols=…` | Contract specs (≤10 symbols, comma-separated) — now **requires auth** |

`ContractDetail` fields that matter for risk: `contract_volume` (100 for
XAUUSD), `min_order_volume`, `max_order_volume`, `step_order_volume`,
`leverage` / `leverages[]`, `price_precision`, `settlement_currency`,
`exchange_rate`.

### 2.2 Accounts & funds

| Method | Endpoint | Notes |
|---|---|---|
| POST | `/tradfi/users` | Create the CFD user (one-time; returns `mt5_uid`) |
| GET | `/tradfi/users/mt5-account` | MT5 account info (`is_register`, `mt5_uid`, leverage, stop-out level) |
| GET | `/tradfi/users/assets` | Balance: `equity`, `balance`, `margin`, `margin_free`, `unrealized_pnl`, `storage`, `outable` |
| POST | `/tradfi/transactions` | Fund transfer: `asset` (USDT only), `change` (≤2 decimals), `type` `deposit`/`withdraw` |
| GET | `/tradfi/transactions` | Transfer history |

### 2.3 Orders

| Method | Endpoint | Notes |
|---|---|---|
| POST | `/tradfi/orders` | Place order (schema below) |
| GET | `/tradfi/orders` | Active orders |
| PUT | `/tradfi/orders/{order_id}` | Modify order |
| DELETE | `/tradfi/orders/{order_id}` | Cancel order |
| GET | `/tradfi/orders/history` | History (earliest query: 1 month) |

`TradFiOrderRequest` (MT5 style):

```
price        — order price (string)
price_type   — "trigger" (limit) | "market"
side         — 1 = sell, 2 = buy
symbol       — e.g. "XAUUSD"
volume       — order quantity in lots (string)
price_tp     — optional take-profit price
price_sl     — optional stop-loss price
```

There is also a newer **spot-style variant** (`TradFiSpot*` models) with
`price_type = market|limit`, `trading_session` (limit orders: `All` only;
market orders: `Regular` only), `time_in_force` (`day`), and `client_order_id` —
preferred for programmatic use if its endpoint paths (`/tradfi/spot/…`) are
confirmed in the live API.

### 2.4 Positions

| Method | Endpoint | Notes |
|---|---|---|
| GET | `/tradfi/positions` | Active positions (`position_id`, direction Long/Short, volume, entry, …) |
| PUT | `/tradfi/positions/{position_id}` | Modify position |
| POST | `/tradfi/positions/{position_id}/close` | Close: `close_type` 1 = partial (`close_volume` required), 2 = full |
| GET | `/tradfi/positions/history` | History (earliest query: 1 month) |

---

## 3. Differences vs. the existing Spot / Futures markets

| Aspect | Spot / Futures | CFD (TradFi) |
|---|---|---|
| API namespace | `/api/v4/spot/*`, `/api/v4/futures/usdt/*` | `/api/v4/tradfi/*` |
| Market data | WebSocket (order book, tickers, klines) | **REST only** — poll tickers (~1s) + klines (1m) |
| Account | Spot / futures balance (USDT) | MT5 CFD account, **USD settlement** |
| Funding | Deposits / exchange transfer | USDT transfer via `/tradfi/transactions` |
| Quantity | Base currency / integer contracts | **Lots** (`volume`, 0.01 min for XAUUSD) |
| Side encoding | `buy` / `sell` | `1` = sell, `2` = buy |
| TP/SL | Separate stop-loss engine | Attachable per order (`price_tp` / `price_sl`) |
| API-key permission | Spot / Futures | **CFD** |

### 3.1 Strategy compatibility

- `MomentumScalper` / `MeanReversionScalper` / `SuperTrendScalper` — kline-driven → **work** with the REST kline feed
- `OrderBookScalper` — needs order book depth → **not available** for CFD (no order-book channel)
- Signal aggregator, cooldown, risk gates (notional / drawdown / rate limiter) all apply unchanged once L7 knows the CFD notional model

---

## 4. Implementation Plan (phases)

```
Phase 1  L1 + L8 (Exchange + Execution)
         - EndpointRouter: /api/v4/tradfi/* paths
         - GateRestClient: TradFi methods (tickers, klines, symbols, detail,
           assets, transfer, orders CRUD, positions, close)
         - OrderExecutor: TradFi order body (lots, side 1/2, price_type,
           trading_session, tif, client_order_id)
Phase 2  L3 (Market Data)
         - RestFeed: ticker + kline polling loop (replaces WS for CFD)
Phase 3  L7 (Risk)
         - Notional = volume × contract_volume(100) × price
         - Margin = notional / leverage; leverage config (20–500)
         - Reuse DrawdownGuard / rate limiter / reservation flow
Phase 4  Control plane + config
         - trading.toml: `market_type = "cfd"`, symbol `XAUUSD`
         - get_market / get_positions / get_account wired to TradFi endpoints
Phase 5  Tests
         - Unit tests per phase; full suite stays green
```

Pre-requisites (done for the operator's account): CFD user created,
USDT transferred into the CFD account (81.86 USD present), API key with the
**CFD** permission checked.

---

## 5. Sources

- Gate API v4 official docs: <https://www.gate.tr/docs/developers/apiv4/en/>
- Official SDKs: <https://github.com/gate/gateapi-python> (`TradFiApi`), <https://github.com/gate/gateapi-nodejs>
- Hummingbot Gate.io integration notes (API-key permissions): <https://hummingbot.org/exchanges/gate-io/>
