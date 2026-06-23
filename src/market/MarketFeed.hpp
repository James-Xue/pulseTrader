#pragma once
// market_feed.hpp — Layer 3 Market Data dispatcher
//
// Integrates all L3 components (TickerCache, SymbolRegistry, KlineBuffer, OrderBookManager)
// and subscribes to Gate.io WebSocket channels to route incoming events to the appropriate
// data structure. Supports both spot and futures markets via MarketType parameter.
//
// Usage:
//   MarketFeed feed(ws_client, rest_client);                    // spot (default)
//   MarketFeed futures_feed(ws_client, rest_client, Futures);   // futures
//   feed.start({"BTC_USDT", "ETH_USDT"});
//   auto ticker = feed.tickerCache().get("BTC_USDT");
//   auto book = feed.orderbookManager().topBids("BTC_USDT", 5);
//   feed.stop();

#include "core/types.hpp"
#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"
#include "market/KlineBuffer.hpp"
#include "market/OrderBookManager.hpp"
#include "market/SymbolRegistry.hpp"
#include "market/TickerCache.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::market
{

// ---------------------------------------------------------------------------
// FeedStats — snapshot of MarketFeed event counters
//
// All counters are monotonically increasing since MarketFeed::start().
// Thread-safe: each counter is an independent std::atomic with relaxed ordering.
// The snapshot itself is NOT atomic (each field is read independently) —
// this is acceptable for monitoring/logging where slight inconsistency
// between fields is harmless.
// ---------------------------------------------------------------------------
struct FeedStats
{
    std::uint64_t ticker_count;    ///< Total ticker updates successfully processed.
    std::uint64_t orderbook_count; ///< Total order book updates successfully processed.
    std::uint64_t kline_count;     ///< Total K-line updates successfully processed.
};

// ---------------------------------------------------------------------------
// MarketFeed — dispatcher that wires WS events to L3 components
// ---------------------------------------------------------------------------
class MarketFeed
{
  public:
    /// Construct a MarketFeed with references to the WS and REST clients.
    ///
    /// Does NOT start subscriptions — call start() explicitly.
    /// MarketType selects which WS channels to subscribe to (spot.* vs futures.*).
    MarketFeed(exchange::GateWsClient &ws_client, exchange::GateRestClient &rest_client,
               MarketType market_type = MarketType::Spot);

    /// Start subscribing to market data channels for the given symbols.
    ///
    /// Subscribes to:
    ///   - spot.tickers (real-time price updates)
    ///   - spot.order_book (incremental order book, 10 levels, 100ms interval)
    ///   - spot.candlesticks (1-minute K-lines)
    ///
    /// Also loads symbol metadata from REST (SymbolRegistry).
    void start(const std::vector<Symbol> &symbols);

    /// Stop all subscriptions and clean up.
    void stop();

    /// Access the ticker cache (read-only for strategy threads).
    [[nodiscard]] TickerCache &tickerCache();

    /// Access the symbol registry (read-only after start()).
    [[nodiscard]] SymbolRegistry &symbolRegistry();

    /// Access the K-line buffer (read-only for strategy threads).
    ///
    /// Note: KlineBuffer is per-symbol. Use getKlineBuffer(symbol) for a specific symbol.
    [[nodiscard]] KlineBuffer &getKlineBuffer(const Symbol &symbol);

    /// Access the order book manager (read-only for strategy threads).
    [[nodiscard]] OrderBookManager &orderbookManager();

    /// Return a snapshot of the event counters.
    ///
    /// Thread-safe (relaxed atomic loads). Suitable for periodic polling
    /// from a monitoring thread — NOT for precise per-event accounting.
    [[nodiscard]] FeedStats stats() const;

  private:
    exchange::GateWsClient &m_wsClient;
    exchange::GateRestClient &m_restClient;
    MarketType m_marketType;

    TickerCache m_tickerCache;
    SymbolRegistry m_symbolRegistry;
    OrderBookManager m_orderbookManager;
    std::unordered_map<Symbol, KlineBuffer> m_klineBuffers; ///< Per-symbol K-line buffers.

    std::vector<Symbol> m_subscribedSymbols;

    // --- Event counters (relaxed atomics, incremented on WS I/O thread) ---
    //
    // These use std::memory_order_relaxed because:
    //   1. They are monitoring counters, not synchronization primitives
    //   2. The only ordering guarantee needed is "increment happens before read"
    //      which is satisfied by the atomic itself on x86 (strongly ordered)
    //   3. On ARM, relaxed avoids expensive barrier instructions on the hot path
    std::atomic<std::uint64_t> m_tickerCount{ 0 };
    std::atomic<std::uint64_t> m_orderbookCount{ 0 };
    std::atomic<std::uint64_t> m_klineCount{ 0 };

    /// Parse a ticker update JSON and store in TickerCache.
    void onTickerUpdate(const nlohmann::json &result, const nlohmann::json &full_frame);

    /// Parse an order book update JSON and apply to OrderBookManager.
    void onOrderbookUpdate(const nlohmann::json &result, const nlohmann::json &full_frame);

    /// Parse a K-line update JSON and push to the appropriate KlineBuffer.
    void onKlineUpdate(const nlohmann::json &result, const nlohmann::json &full_frame);
};

} // namespace pulse::market
