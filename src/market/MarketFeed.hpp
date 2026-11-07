#pragma once
// market_feed.hpp — Layer 3 Market Data dispatcher
//
// Integrates all L3 components (TickerCache, SymbolRegistry, KlineBuffer, OrderBookManager)
// and subscribes to Gate.io WebSocket channels to route incoming events to the appropriate
// data structure. Supports spot, futures and TradFi CFD markets via MarketType parameter.
// CFD has no WebSocket market data — MarketType::Cfd switches the feed to a REST
// polling loop (ticker ~1s, klines ~60s) driven by a dedicated std::jthread.
//
// Usage:
//   MarketFeed feed(&ws_client, rest_client);                    // spot (default)
//   MarketFeed futures_feed(&ws_client, rest_client, Futures);   // futures
//   MarketFeed cfd_feed(nullptr, rest_client, Cfd, &rest_mutex); // CFD (REST poll)
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
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::market
{

class MarketDataSink; // Forward declaration — see MarketDataSink.hpp.

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
    /// Construct a MarketFeed with the WS and REST clients.
    ///
    /// Does NOT start subscriptions — call start() explicitly.
    /// MarketType selects which WS channels to subscribe to (spot.* vs futures.*).
    /// ws_client may be nullptr for MarketType::Cfd (no WebSocket channel exists —
    /// the feed runs a REST polling loop instead).
    /// rest_mutex (optional) serialises REST polling with the rest of the engine
    /// (GateRestClient is not thread-safe); ignored for WS-based markets.
    MarketFeed(exchange::GateWsClient *ws_client, exchange::GateRestClient &rest_client,
               MarketType market_type = MarketType::Spot, std::mutex *rest_mutex = nullptr);

    /// Start subscribing to market data channels for the given symbols.
    ///
    /// Subscribes to:
    ///   - spot.tickers (real-time price updates)
    ///   - spot.order_book (incremental order book, 10 levels, 100ms interval)
    ///   - spot.candlesticks (1-minute K-lines)
    ///
    /// MarketType::Cfd instead spawns a REST poll thread (ticker ~1s, 1m klines ~60s).
    ///
    /// Also loads symbol metadata from REST (SymbolRegistry).
    void start(const std::vector<Symbol> &symbols);

    /// Stop all subscriptions / poll thread and clean up.
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

    /// Attach a market data sink (ticker/kline events). The sink is invoked
    /// on the feed's I/O thread and MUST NOT block (see MarketDataSink.hpp).
    ///
    /// May be null (default) — no events are dispatched. Call before start()
    /// or while the feed is stopped; the pointer is not owned by the feed.
    void setMarketDataSink(MarketDataSink *sink);

    /// Parse a ticker update JSON (WS spot/futures ticker channel).
    ///
    /// Test/diagnostic seam: kept public so sink dispatch can be unit-tested
    /// without a live WebSocket connection.
    void onTickerUpdate(const nlohmann::json &result, const nlohmann::json &full_frame);

    /// Parse a K-line update JSON (WS spot/futures candlestick channel).
    ///
    /// Test/diagnostic seam: kept public so sink dispatch can be unit-tested
    /// without a live WebSocket connection.
    void onKlineUpdate(const nlohmann::json &result, const nlohmann::json &full_frame,
                       const std::string &fallback_symbol = "");

    /// Parse a TradFi ticker object into a Ticker (pure, unit-testable).
    ///
    /// Expected shape (probe-verified 2026-08-15):
    ///   { "last_price": "4376.45", "bid_price": "...", "ask_price": "...",
    ///     "price_change": "0.58", ... } — all values are strings.
    /// symbol is supplied by the caller (the ticker object itself may omit it).
    [[nodiscard]] static std::optional<Ticker> parseCfdTicker(const nlohmann::json &obj,
                                                              const Symbol &symbol);

    /// Parse a TradFi 1m kline object into a Kline (pure, unit-testable).
    ///
    /// Expected shape (probe-verified 2026-08-15):
    ///   { "o": "4375.88", "h": "4375.91", "l": "4375.4", "c": "4375.62",
    ///     "t": 1786740960 } — t is Unix seconds; no volume field exists.
    [[nodiscard]] static std::optional<Kline> parseCfdKline(const nlohmann::json &obj);

  private:
    exchange::GateWsClient *m_wsClient; ///< Null for MarketType::Cfd (REST-poll mode).
    exchange::GateRestClient &m_restClient;
    MarketType m_marketType;
    std::mutex *m_restMutex;            ///< Shared REST mutex for the CFD poll thread (may be null).
    std::jthread m_pollThread;          ///< REST poll thread (CFD only).

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

    MarketDataSink *m_dataSink{ nullptr }; ///< Optional event consumer (not owned).

    /// Parse an order book update JSON and apply to OrderBookManager.
    void onOrderbookUpdate(const nlohmann::json &result, const nlohmann::json &full_frame);

    /// REST polling loop for MarketType::Cfd (no WebSocket channel).
    ///
    /// Polls the ticker once per second and 1m klines once per minute
    /// (first kline fetch happens immediately as backfill). Holds m_restMutex
    /// only around each request — never while sleeping.
    void pollLoop(std::stop_token stoken);

};

} // namespace pulse::market
