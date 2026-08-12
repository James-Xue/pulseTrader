"""market_data — ticker polling → synthetic 10s bars → volume profile → levels.

The Gate.io TradFi API does NOT expose historical klines for XAUUSD via REST
(`/tradfi/symbols/XAUUSD/klines` returns 400 INVALID_ARGUMENT for every query
param combo) and the documented WS candle channels (`tradfi.candlesticks`,
`mtickers.candlesticks`) are unknown. To keep the volume-profile strategy
runnable, we **synthesize 10-second bars by polling the ticker** every second
and aggregating price-only data. Volume in the profile is therefore
**time-at-price** (seconds the last_price spent in each $0.10 bin) rather
than real tick volume — it is a useful proxy for "how long did the market
rest at this level" which is the meaningful signal for mean-reversion.

Submodules:
    ticker         — REST polling of /tradfi/symbols/XAUUSD/tickers
    klines         — synthetic 10s bar aggregator from tick stream
    volume_profile — rolling deque of bars → binned time-at-price profile
    levels         — split top-N nodes into support (below mid) / resistance
"""