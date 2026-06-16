#pragma once
// market_feed.hpp — Layer 3 Market Data dispatcher
//
// Integrates all L3 components (TickerCache, SymbolRegistry, KlineBuffer, OrderBookManager)
// and subscribes to Gate.io WebSocket channels to route incoming events to the appropriate
// data structure.
//
// Usage:
//   MarketFeed feed(ws_client, rest_client);
//   feed.start({"BTC_USDT", "ETH_USDT"});
//   auto ticker = feed.ticker_cache().get("BTC_USDT");
//   auto book = feed.orderbook_manager().top_bids("BTC_USDT", 5);
//   feed.stop();

#include "pulse/exchange/gate_rest_client.hpp"
#include "pulse/exchange/gate_ws_client.hpp"
#include "pulse/market/kline_buffer.hpp"
#include "pulse/market/orderbook_manager.hpp"
#include "pulse/market/symbol_registry.hpp"
#include "pulse/market/ticker_cache.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::market
{

// ---------------------------------------------------------------------------
// MarketFeed — dispatcher that wires WS events to L3 components
// ---------------------------------------------------------------------------
class MarketFeed
{
  public:
    /// Construct a MarketFeed with references to the WS and REST clients.
    ///
    /// Does NOT start subscriptions — call start() explicitly.
    MarketFeed(exchange::GateWsClient &ws_client, exchange::GateRestClient &rest_client);

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

  private:
    exchange::GateWsClient &ws_client_;
    exchange::GateRestClient &rest_client_;

    TickerCache ticker_cache_;
    SymbolRegistry symbol_registry_;
    OrderBookManager orderbook_manager_;
    std::unordered_map<Symbol, KlineBuffer> kline_buffers_; ///< Per-symbol K-line buffers.

    std::vector<Symbol> subscribed_symbols_;

    /// Parse a ticker update JSON and store in TickerCache.
    void on_ticker_update(const nlohmann::json &result, const nlohmann::json &full_frame);

    /// Parse an order book update JSON and apply to OrderBookManager.
    void on_orderbook_update(const nlohmann::json &result, const nlohmann::json &full_frame);

    /// Parse a K-line update JSON and push to the appropriate KlineBuffer.
    void on_kline_update(const nlohmann::json &result, const nlohmann::json &full_frame);
};

} // namespace pulse::market
