#pragma once
// ticker_cache.hpp — Thread-safe ticker cache for Layer 3 Market Data
//
// Stores the latest ticker data (best bid/ask, last price, 24h volume) per symbol.
// Updates and reads are protected by a shared_mutex (readers-writer lock).
//
// Thread safety:
//   - update() can be called from the WebSocket I/O thread (exclusive write lock)
//   - get() can be called from strategy threads concurrently (shared read lock)
//   - Multiple readers can proceed simultaneously; writers are exclusive

#include "core/types.hpp"

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::market
{

// ---------------------------------------------------------------------------
// Ticker — latest market snapshot for one symbol
// ---------------------------------------------------------------------------
struct Ticker
{
    std::string symbol;       ///< Trading pair, e.g. "BTC_USDT".
    Price last;               ///< Last traded price.
    Price bid;                ///< Best bid price.
    Price ask;                ///< Best ask price.
    Quantity volume_24h;      ///< 24-hour trading volume in base currency.
    Price change_pct;         ///< 24-hour price change percentage.
    std::int64_t timestamp;   ///< Unix timestamp in milliseconds.

    // Futures-specific fields (zero for spot).
    Price mark_price;         ///< Mark price (futures only).
    Price index_price;        ///< Index price (futures only).
    double funding_rate;      ///< Funding rate (futures only, e.g. 0.0001 = 0.01%).

    /// Default constructor — zero-initializes all fields.
    Ticker()
        : symbol{}
        , last{ 0.0 }
        , bid{ 0.0 }
        , ask{ 0.0 }
        , volume_24h{ 0.0 }
        , change_pct{ 0.0 }
        , timestamp{ 0 }
        , mark_price{ 0.0 }
        , index_price{ 0.0 }
        , funding_rate{ 0.0 }
    {
    }
};

// ---------------------------------------------------------------------------
// TickerCache — thread-safe storage for latest ticker per symbol
//
// Usage:
//   TickerCache cache;
//   cache.update("BTC_USDT", ticker);  // from WS thread
//   auto opt = cache.get("BTC_USDT");  // from strategy thread
//   if (opt) { use(opt->last); }
// ---------------------------------------------------------------------------
class TickerCache
{
  public:
    /// Update the ticker for a symbol.
    ///
    /// If this is the first update for the symbol, a new entry is created.
    /// Subsequent updates replace the existing ticker.
    ///
    /// Parameters:
    ///   1. symbol — trading pair (e.g. "BTC_USDT")
    ///   2. ticker — new ticker data (copied)
    void update(const Symbol &symbol, const Ticker &ticker);

    /// Retrieve the latest ticker for a symbol (thread-safe read).
    ///
    /// Returns std::nullopt if the symbol has never been updated.
    [[nodiscard]] std::optional<Ticker> get(const Symbol &symbol) const;

    /// Check if a symbol has been updated at least once.
    [[nodiscard]] bool contains(const Symbol &symbol) const;

    /// Returns the number of symbols currently cached.
    [[nodiscard]] std::size_t size() const;

    /// Returns a vector of all symbols currently cached.
    /// Thread-safe: takes shared read lock.
    [[nodiscard]] std::vector<Symbol> symbols() const;

  private:
    mutable std::shared_mutex mutex_; ///< Protects the cache map.
    std::unordered_map<Symbol, Ticker> cache_;
};

} // namespace pulse::market
