#pragma once
// orderbookManager.hpp — Incremental order book reconstruction (Layer 3 Market Data)
//
// Maintains a full sorted order book per symbol by applying snapshot and delta updates
// from the Gate.io WebSocket feed. Validates sequence numbers to detect gaps and
// triggers re-subscription when consistency cannot be guaranteed.
//
// Gate.io order book channel format:
//   - Snapshot: {"lastUpdateId": N, "bids": [[price, qty], ...], "asks": [[price, qty], ...]}
//   - Delta: {"lastUpdateId": N+1, "bids": [[price, qty], ...], "asks": [[price, qty], ...]}
//   - Quantity = 0 means remove the price level
//
// Thread safety:
//   - applySnapshot() and applyDelta() are called from the WebSocket I/O thread
//   - get() and topBids/topAsks() are called from strategy threads (shared read lock)

#include "core/types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::market
{

// ---------------------------------------------------------------------------
// OrderBookLevel — one price level in the order book
// ---------------------------------------------------------------------------
struct OrderBookLevel
{
    Price price;
    Quantity quantity;
};

// ---------------------------------------------------------------------------
// OrderBook — full sorted order book for one symbol
// ---------------------------------------------------------------------------
struct OrderBook
{
    Symbol symbol;
    std::map<Price, Quantity, std::greater<Price>> bids; ///< Sorted descending by price.
    std::map<Price, Quantity, std::less<Price>> asks;    ///< Sorted ascending by price.
    std::uint64_t sequence_id;                            ///< Last update ID from exchange.
    std::int64_t timestamp;                               ///< Unix timestamp in milliseconds.

    OrderBook()
        : symbol{}
        , bids{}
        , asks{}
        , sequence_id{ 0 }
        , timestamp{ 0 }
    {
    }
};

// ---------------------------------------------------------------------------
// OrderBookManager — snapshot + delta incremental updates with sequence validation
//
// Usage:
//   OrderBookManager manager;
//   manager.setResubscribeCallback([](const Symbol& s) { ... });
//   manager.applySnapshot("BTC_USDT", snapshot_json);
//   manager.applyDelta("BTC_USDT", delta_json);
//   auto book = manager.get("BTC_USDT");
//   auto top5_bids = manager.topBids("BTC_USDT", 5);
// ---------------------------------------------------------------------------
class OrderBookManager
{
  public:
    /// Callback type for re-subscription requests (triggered on sequence gap).
    using ResubscribeCallback = std::function<void(const Symbol &symbol)>;

    /// Set the callback invoked when a sequence gap is detected.
    ///
    /// The callback should re-subscribe to the order book channel for the given symbol.
    void setResubscribeCallback(ResubscribeCallback callback);

    /// Apply a full order book snapshot (replaces any existing data).
    ///
    /// Parameters:
    ///   1. symbol   — trading pair (e.g. "BTC_USDT")
    ///   2. snapshot — JSON object with "lastUpdateId", "bids", "asks" fields
    ///
    /// The snapshot replaces the entire order book for the symbol.
    void applySnapshot(const Symbol &symbol, const nlohmann::json &snapshot);

    /// Apply an incremental delta update.
    ///
    /// Parameters:
    ///   1. symbol — trading pair
    ///   2. delta  — JSON object with "lastUpdateId", "bids", "asks" fields
    ///
    /// Validates the sequence number: if delta.lastUpdateId != last_sequence + 1,
    /// a sequence gap is detected and the resubscribe callback is invoked.
    ///
    /// For each bid/ask level in the delta:
    ///   - If quantity > 0: insert or update the level
    ///   - If quantity == 0: remove the level
    void applyDelta(const Symbol &symbol, const nlohmann::json &delta);

    /// Retrieve a copy of the full order book (thread-safe).
    ///
    /// Returns std::nullopt if no snapshot has been applied yet.
    [[nodiscard]] std::optional<OrderBook> get(const Symbol &symbol) const;

    /// Retrieve the top N bid levels (highest prices first).
    [[nodiscard]] std::vector<OrderBookLevel> topBids(const Symbol &symbol, std::size_t n) const;

    /// Retrieve the top N ask levels (lowest prices first).
    [[nodiscard]] std::vector<OrderBookLevel> topAsks(const Symbol &symbol, std::size_t n) const;

    /// Check if a symbol has an active order book (snapshot applied).
    [[nodiscard]] bool contains(const Symbol &symbol) const;

  private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<Symbol, OrderBook> m_books;
    std::unordered_map<Symbol, std::uint64_t> m_lastSequence;
    ResubscribeCallback m_resubscribeCallback;

    /// Parse a JSON array of [price, quantity] pairs into a map.
    template <typename Compare>
    static void parseLevels(std::map<Price, Quantity, Compare> &out, const nlohmann::json &levels_json);

    /// Apply delta updates to a map of price levels.
    template <typename Compare>
    static void applyDeltaLevels(std::map<Price, Quantity, Compare> &book, const nlohmann::json &levels_json);
};

} // namespace pulse::market
