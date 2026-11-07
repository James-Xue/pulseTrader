#pragma once
// market_data_sink.hpp — Push-style consumer interface for market data (Layer 3)
//
// MarketFeed dispatches every incoming ticker/kline update to a single optional
// sink. The sink is invoked on the feed's I/O thread (WebSocket callback or the
// CFD REST poll thread), so the contract is strict:
//
//   - Implementations MUST NOT block. Only O(1) enqueue (e.g. into a ring
//     buffer) is allowed — anything heavier stalls market data ingestion
//     for the whole engine.
//
// The sink pointer is not owned by MarketFeed (raw pointer, may be null).
// setMarketDataSink() must be called before start() or while the feed is stopped.

#include "core/types.hpp"
#include "market/KlineBuffer.hpp"
#include "market/TickerCache.hpp"

namespace pulse::market
{

// ---------------------------------------------------------------------------
// MarketDataSink — push-style consumer of market data events
// ---------------------------------------------------------------------------
class MarketDataSink
{
  public:
    virtual ~MarketDataSink() = default;

    /// Called on the feed I/O thread for every ticker update.
    /// MUST NOT block (O(1) enqueue only).
    virtual void onTicker(const Symbol &symbol, MarketType market_type,
                          const Ticker &ticker) = 0;

    /// Called on the feed I/O thread for every kline update.
    /// MUST NOT block (O(1) enqueue only).
    virtual void onKline(const Symbol &symbol, MarketType market_type,
                         const Kline &kline) = 0;
};

} // namespace pulse::market
