# Technical memo: managing TP/SL trigger orders on Gate futures positions

> **Generated**: 2026-08-16 (verified in live trading: successfully adjusted the SKHY_USDT short TP from 150 to 151)
> **Scope**: Gate.io futures contracts (`/futures/usdt/...`), USDT-settled; does not apply to spot/CFD
> **Key conclusion**: a position's TP/SL are **standalone price-trigger orders**; the position's `close_order` field misses them (a null return ≠ no protection). The PUT endpoint for modifying trigger orders does not work — **the correct approach is delete-old + create-new**

---

## 1. The Three Key Endpoints

| Endpoint | Method | Purpose |
|---|---|---|
| `/futures/usdt/positions/{contract}` | GET | Position (includes `close_order`/`pending_orders` fields, **does not show trigger orders**) |
| `/futures/usdt/price_orders?contract=X&status=open` | GET | **⚠️ The real TP/SL query entry point** |
| `/futures/usdt/price_orders/{id}` | DELETE | Delete a trigger order |
| `/futures/usdt/price_orders` | POST | Create a trigger order |
| `/futures/usdt/price_orders/{id}` | PUT | ⚠️ Modify — **in practice returns `AUTO_INVALID_PARAM_ORDER_ID`; unusable** |

## 2. Trigger Order Field Semantics

```json
{
  "id": "2088850508826546176",
  "status": "open",
  "order_type": "close-short-position",   // close a short; for a long it is close-long-position
  "direction": "long",                    // closing a short = buy back (direction long)
  "trigger": {
    "price": "151",
    "rule": 2,                            // 1=trigger when price >= X (stop-loss direction) 2=trigger when price <= X (take-profit direction)
    "price_type": 0,                      // 0=last traded price 1=mark price 2=index price
    "expiration": 0                       // 0=valid indefinitely
  },
  "initial": {
    "contract": "SKHY_USDT",
    "size": 0,
    "price": "0",
    "tif": "ioc",                         // IOC market order after the trigger
    "is_reduce_only": true,
    "auto_size": "close"                  // auto-close the position quantity on trigger
  }
}
```

**Rule of thumb**: for a short position `rule=1` (price crosses up) = SL, `rule=2` (price crosses down) = TP; the reverse for a long position.

## 3. Reusable bash Signing Function (tested)

```bash
source /path/to/.env   # contains GATE_MAINNET_API_KEY / GATE_MAINNET_API_SECRET

# Signature format: METHOD\nPATH\nQUERY\nsha512hex(BODY)\nTIMESTAMP  <- the timestamp goes last!
# body hash: for an empty body use the sha512 hex of the empty string
sign_req() { # $1=METHOD $2=PATH $3=QUERY $4=BODY
  local ts hashed sign
  ts=$(date +%s)
  hashed=$(printf '%s' "$4" | openssl dgst -sha512 | awk '{print $2}')
  sign=$(printf '%s\n%s\n%s\n%s\n%s' "$1" "$2" "$3" "$hashed" "$ts" \
    | openssl dgst -sha512 -hmac "$GATE_MAINNET_API_SECRET" | awk '{print $2}')
  local url="https://api.gateio.ws${2}"; [ -n "$3" ] && url="${url}?${3}"
  curl -s -X "$1" -H "KEY: $GATE_MAINNET_API_KEY" -H "Timestamp: $ts" \
    -H "SIGN: $sign" -H "Content-Type: application/json" \
    ${4:+-d "$4"} "$url"
}
```

### 3.1 Querying the TP/SL on a Position

```bash
sign_req GET "/api/v4/futures/usdt/price_orders" "contract=SKHY_USDT&status=open" "" \
  | python3 -m json.tool
```

### 3.2 Adjusting TP/SL = Delete Old, Create New (the core pattern)

```bash
TP_ID=2088850508826546176   # ID of the old trigger order to replace

# (1) Delete the old one
sign_req DELETE "/api/v4/futures/usdt/price_orders/$TP_ID" "" "" 

# (2) Create the new one (just change trigger.price / rule; copy every other field from the original order)
BODY='{"initial":{"contract":"SKHY_USDT","size":0,"price":"0","tif":"ioc","text":"app","iceberg":0,"is_close":false,"is_reduce_only":true,"auto_size":"close","amount":"0","iceberg_amount":"0"},"trigger":{"strategy_type":0,"price_type":0,"price":"151","rule":2,"expiration":0},"order_type":"close-short-position"}'
sign_req POST "/api/v4/futures/usdt/price_orders" "" "$BODY"

# (3) Verify
sign_req GET "/api/v4/futures/usdt/price_orders" "contract=SKHY_USDT&status=open" "" \
  | python3 -c "import json,sys; [print(o['trigger']['price'], 'rule='+str(o['trigger']['rule']), o['status']) for o in json.load(sys.stdin)]"
```

## 4. Pitfall List (all verified in live trading)

1. **The `close_order` field does not show trigger orders**: `close_order: null` / `pending_orders: 0` in the position API ≠ no protection — trigger orders live in a separate `price_orders` endpoint and must be queried separately. The first version of the SKHY analysis report therefore wrongly concluded "no protective orders".
2. **PUT modification does not work**: `PUT /price_orders/{id}` returns `AUTO_INVALID_PARAM_ORDER_ID` (even with a complete, valid body) → use delete-old-create-new instead.
3. **The signature timestamp goes last**: the Gate v4 signature string is `METHOD\nPATH\nQUERY\nsha512hex(BODY)\nTIMESTAMP`, not a `timestamp+method+path` concatenation. When BODY is empty, use the sha512 of the empty string.
4. **Shell variable name trap**: do not use `PATH` as the variable name for the request path (it overrides the environment variable PATH, making date/openssl/curl all "not found"). Use `REQ_PATH`.
5. **Trigger basis**: the default `price_type=0` triggers on the **last traded price** plus an IOC market close — in fast wick-driven conditions there is still slippage after the trigger, and in extreme cases execution can land near the liquidation price.
6. **Direction matching**: a `close-short-position` trigger order is only effective while a short position exists; what happens with no position needs live testing (confirm the position exists before placing the order).
7. **Mainnet real money**: delete-old-create-new is a milliseconds-scale window, but any failed step leaves an intermediate state of "SL only, no TP" — creating the new order before deleting the old one shortens the risk window further (do not delete the old order until the new one is created successfully).

## 5. Appendix: Distance Relationship Between SL/TP and Liquidation (position-protection design reference)

Short position (SKHY example): entry 161.84, current price 168.46, liquidation 173.02
- SL 170.5 (rule=1): maximum loss locked in (170.5-161.84)×4000×0.01 ≈ **-346.5 USDT** (triggers 1.2% before liquidation)
- TP 151 (rule=2): target profit (161.84-151)×4000×0.01 ≈ **+433.5 USDT**
- Experience: the SL should be placed **below** the liquidation line (short) / **above** it (long) to leave headroom; if it is too close (e.g. +1.21% this time) it is easily swept away by intraday noise in a trending market

---

*Technical memo, 2026-08-16. Corresponding live case: SKHY_USDT short TP adjusted from 150 to 151 (archive archive/2026-08-16/125200_...)*
