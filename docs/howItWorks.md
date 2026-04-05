# pulseTrader — How It Works

> A narrative walkthrough of the system design.  
> For the full technical specification, see [architecture.md](architecture.md).

---

## A Single BTC Trade, From Start to Finish

Imagine it is late at night and BTC starts moving. Here is what pulseTrader does, one step at a time.

---

### Step 1 — Exchange Layer receives raw market data

Gate.io pushes a WebSocket frame:

```
BTC/USDT last price: 65,000 → 65,200   (buy pressure surging)
```

`GateWsClient` receives the raw JSON frame and dispatches it upward.

---

### Step 2 — Market Layer builds an in-memory view

`MarketFeed` routes the event to three sub-components simultaneously:

| Component | What it does |
|---|---|
| `TickerCache` | Atomically stores the latest price 65,200 — no locks, single instruction |
| `OrderBookManager` | Updates the sorted bid/ask depth levels |
| `KlineBuffer` | Extends the current 1-minute candle's high |

The strategy threads can now read any of these with minimal latency.

---

### Step 3 — Strategy Layer decides whether to trade

All active strategies have their `on_tick()` hook called:

| Strategy | Observation | Signal | Confidence |
|---|---|---|---|
| `MomentumScalper` | EMA has crossed up | **BUY** | 0.8 |
| `OrderBookScalper` | Bid/ask volume ratio = 2.1 | **BUY** | 0.6 |
| `MeanReversionScalper` | Price not at Bollinger extreme | FLAT | — |

`SignalAggregator` applies weighted voting across the three signals and produces a consolidated output:

```
→ BUY  0.01 BTC
```

---

### Step 4 — Risk Layer acts as a gate

Every order must pass through `RiskManager` before it can be submitted:

```
PositionManager  : current BTC exposure is within limit      ✓
DrawdownGuard    : today's loss is within threshold          ✓
OrderRateLimiter : token bucket has capacity                 ✓

→ Order approved
```

If any check fails, the order is reduced in size or rejected outright, and the reason is logged.

---

### Step 5 — Execution Layer places the order

`OrderExecutor` submits a limit order via the Gate.io REST API:

```
Side:     BUY
Price:    65,190 USDT
Quantity: 0.01 BTC
```

`OrderTracker` then listens on the WebSocket for a fill event. If the WebSocket misses it, the tracker falls back to polling the REST order status endpoint.

---

### Step 6 — Logging Layer records everything

Once the order is filled, an `ExecutionReport` is generated and persisted:

```
Slippage : +2 USDT vs mid-price at submission
Fees     : 0.065 USDT
Latency  : 18 ms (submission → fill acknowledgement)
```

`TradeRecorder` writes the record to CSV. `MetricsCollector` updates the rolling win-rate, PnL, and Sharpe ratio.

---

## Meanwhile, in the Background (every 5 minutes)

The AI analysis cycle runs completely independently of the trade hot path.

```
HeartbeatScheduler  fires an OnBeat task
        │
        ▼
TaskQueue  (background worker thread)
        │
        ▼
AIAnalyzer collects signals:
  ├── TwitterFeed  → recent BTC-related tweets from X API v2
  ├── NewsFeed     → latest crypto headlines (NewsAPI / CryptoPanic)
  └── PromptBuilder → assembles a structured prompt:
                       current market snapshot
                     + recent K-line data
                     + tweet excerpts
                     + current strategy parameters
        │
        ▼
AIClient  calls Claude or GPT-4o and receives a fixed JSON response:

  {
    "sentiment":           "bullish",
    "direction_bias":      0.65,
    "volatility_forecast": "medium",
    "confidence":          0.85,
    "recommended_param_deltas": {
      "position_size_delta":  +0.002,
      "stop_loss_delta":      -0.001
    }
  }

        │
        ▼
ParamAdvisor  validates the deltas against safety bounds,
              then atomically writes the new values into StrategyParams.

→ The next trade will automatically use a slightly larger position size.
   No restart required. No locks held during the update.
```

---

## Three Core Design Ideas (Plain Language)

### 1. Layered isolation

Each layer only knows about its immediate neighbours. The Exchange layer has no idea strategies exist. The Strategy layer has no idea how HTTP signing works. This means you can rewrite or test any single layer without touching the others.

### 2. The hot path never waits for AI

AI inference typically takes 1–5 seconds. If the system waited for it on the same thread that processes WebSocket frames, it would miss ticks and see stale order books. pulseTrader runs the entire AI cycle on a separate background thread. Market data, strategy evaluation, risk checks, and order submission all proceed without interruption.

### 3. AI tunes the knobs, not the trades

The AI does not place orders directly. It reads the market context every 5 minutes and adjusts a small set of numerical parameters — position size, stop distance, profit target. The strategy algorithms remain in control of every individual trade decision. This separation means a bad AI response at most shifts a parameter by a bounded amount; it cannot bypass the risk layer or submit arbitrary orders.

---

## Layer Map at a Glance

```
Gate.io ──► [1 Exchange] ──► [2 Market Data] ──► [3 Strategy Engine]
                                                         │
                                              [6 Risk Management]
                                                         │
                                              [7 Order Execution]
                                                         │
                                            [8 Logging & Monitoring]

Every 5 min:
[4 Heartbeat] ──► [5 AI Analysis] ──► atomic writes ──► [3 Strategy Engine]
```

---

## Where to Go Next

| Goal | Document |
|---|---|
| Full module API reference and threading model | [architecture.md](architecture.md) |
| Build instructions and dependency setup | [../README.md](../README.md) |
