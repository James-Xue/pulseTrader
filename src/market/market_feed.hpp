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
//   auto ticker = feed.ticker_cache().get("BTC_USDT");
//   auto book = feed.orderbook_manager().top_bids("BTC_USDT", 5);
//   feed.stop();

#include "core/types.hpp"
#include "exchange/gate_rest_client.hpp"
#include "exchange/gate_ws_client.hpp"
#include "market/kline_buffer.hpp"
#include "market/orderbook_manager.hpp"
#include "market/symbol_registry.hpp"
#include "market/ticker_cache.hpp"

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
    [[nodiscard]] TickerCache &ticker_cache();

    /// Access the symbol registry (read-only after start()).
    [[nodiscard]] SymbolRegistry &symbol_registry();

    /// Access the K-line buffer (read-only for strategy threads).
    ///
    /// Note: KlineBuffer is per-symbol. Use get_kline_buffer(symbol) for a specific symbol.
    [[nodiscard]] KlineBuffer &get_kline_buffer(const Symbol &symbol);

    /// Access the order book manager (read-only for strategy threads).
    [[nodiscard]] OrderBookManager &orderbook_manager();

    /// Return a snapshot of the event counters.
    ///
    /// Thread-safe (relaxed atomic loads). Suitable for periodic polling
    /// from a monitoring thread — NOT for precise per-event accounting.
    [[nodiscard]] FeedStats stats() const;

  private:
    exchange::GateWsClient &ws_client_;
    exchange::GateRestClient &rest_client_;
    MarketType market_type_;

    TickerCache ticker_cache_;
    SymbolRegistry symbol_registry_;
    OrderBookManager orderbook_manager_;
    std::unordered_map<Symbol, KlineBuffer> kline_buffers_; ///< Per-symbol K-line buffers.

    std::vector<Symbol> subscribed_symbols_;

    // --- Event counters (relaxed atomics, incremented on WS I/O thread) ---
    //
    // These use std::memory_order_relaxed because:
    //   1. They are monitoring counters, not synchronization primitives
    //   2. The only ordering guarantee needed is "increment happens before read"
    //      which is satisfied by the atomic itself on x86 (strongly ordered)
    //   3. On ARM, relaxed avoids expensive barrier instructions on the hot path
    std::atomic<std::uint64_t> ticker_count_{ 0 };
    std::atomic<std::uint64_t> orderbook_count_{ 0 };
    std::atomic<std::uint64_t> kline_count_{ 0 };

    /// Parse a ticker update JSON and store in TickerCache.
    void on_ticker_update(const nlohmann::json &result, const nlohmann::json &full_frame);

    /// Parse an order book update JSON and apply to OrderBookManager.
    void on_orderbook_update(const nlohmann::json &result, const nlohmann::json &full_frame);

    /// Parse a K-line update JSON and push to the appropriate KlineBuffer.
    void on_kline_update(const nlohmann::json &result, const nlohmann::json &full_frame);
};

} // namespace pulse::market
