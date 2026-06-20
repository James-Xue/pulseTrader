#pragma once
// symbol_registry.hpp — Instrument metadata registry for Layer 3 Market Data
//
// Stores trading instrument metadata (tick size, lot size, min notional) fetched
// from Gate.io REST API. Used to validate order parameters before submission.
// Supports both spot and futures markets via MarketType parameter.
//
// Thread safety:
//   - load_from_rest() is called once at startup (exclusive write lock)
//   - get() and validate_order() are read-only (shared read lock)
//   - Safe for concurrent reads from strategy threads after initialization

#include "core/types.hpp"
#include "exchange/gate_rest_client.hpp"

#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::market
{

// ---------------------------------------------------------------------------
// SymbolInfo — metadata for one trading instrument
// ---------------------------------------------------------------------------
struct SymbolInfo
{
    Symbol symbol;              ///< Trading pair, e.g. "BTC_USDT".
    Price tick_size;            ///< Minimum price increment (e.g. 0.01).
    Quantity lot_size;          ///< Minimum quantity increment (e.g. 0.0001).
    Price min_notional;         ///< Minimum order notional value in quote currency.
    Price min_quote_amount;     ///< Minimum quote currency amount per order.
    Quantity min_base_amount;   ///< Minimum base currency amount per order.
    bool trading_enabled;       ///< Whether trading is currently enabled.

    // Futures-specific fields (defaults make spot instruments work unchanged).
    double quanto_multiplier;   ///< Contract multiplier (e.g. 0.0001 = 1 contract = 0.0001 BTC).
    double leverage_max;        ///< Maximum leverage (e.g. 125).
    double leverage_min;        ///< Minimum leverage (e.g. 1).
    double maintenance_rate;    ///< Maintenance margin rate (e.g. 0.005).
    int funding_interval;       ///< Funding rate interval in seconds (e.g. 28800 = 8h).
    int order_size_min;         ///< Minimum order size in contracts.
    int order_size_max;         ///< Maximum order size in contracts.
    MarketType market_type;     ///< Spot or Futures.

    /// Default constructor — zero-initializes all fields.
    SymbolInfo()
        : symbol{}
        , tick_size{ 0.0 }
        , lot_size{ 0.0 }
        , min_notional{ 0.0 }
        , min_quote_amount{ 0.0 }
        , min_base_amount{ 0.0 }
        , trading_enabled{ false }
        , quanto_multiplier{ 1.0 }
        , leverage_max{ 1.0 }
        , leverage_min{ 1.0 }
        , maintenance_rate{ 0.0 }
        , funding_interval{ 0 }
        , order_size_min{ 0 }
        , order_size_max{ 0 }
        , market_type{ MarketType::Spot }
    {
    }
};

// ---------------------------------------------------------------------------
// SymbolRegistry — metadata store with REST initialization
//
// Usage:
//   SymbolRegistry registry(rest_client);
//   registry.load_from_rest();  // fetch all currency pairs
//   auto info = registry.get("BTC_USDT");
//   if (info && registry.validate_order("BTC_USDT", price, qty)) { ... }
// ---------------------------------------------------------------------------
class SymbolRegistry
{
  public:
    /// Construct a registry with a reference to the REST client.
    ///
    /// Does NOT fetch data — call load_from_rest() explicitly.
    /// MarketType selects which endpoint to load from (spot or futures).
    explicit SymbolRegistry(exchange::GateRestClient &rest_client,
                            MarketType market_type = MarketType::Spot);

    /// Fetch all instruments from Gate.io REST API and populate the registry.
    ///
    /// Spot:    GET /api/v4/spot/currency_pairs
    /// Futures: GET /api/v4/futures/usdt/contracts
    ///
    /// Replaces any existing data (safe to call multiple times for refresh).
    /// Returns true on success, false on network/parse error.
    bool load_from_rest();

    /// Retrieve metadata for a symbol (read-only, thread-safe).
    ///
    /// Returns std::nullopt if the symbol is not in the registry.
    [[nodiscard]] std::optional<SymbolInfo> get(const Symbol &symbol) const;

    /// Validate order parameters against symbol metadata.
    ///
    /// Checks:
    ///   1. Symbol exists and trading is enabled
    ///   2. Price is a multiple of tick_size
    ///   3. Quantity is a multiple of lot_size
    ///   4. Notional value (price * qty) >= min_notional
    ///   5. Base amount >= min_base_amount
    ///   6. Quote amount >= min_quote_amount
    ///
    /// Returns true if all checks pass, false otherwise.
    [[nodiscard]] bool validate_order(const Symbol &symbol, Price price, Quantity qty) const;

    /// Returns the number of symbols in the registry.
    [[nodiscard]] std::size_t size() const;

    /// Returns a vector of all registered symbol names.
    /// Thread-safe: takes shared read lock.
    [[nodiscard]] std::vector<Symbol> symbols() const;

  private:
    exchange::GateRestClient &rest_client_;
    MarketType market_type_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<Symbol, SymbolInfo> symbols_;

    /// Parse a single currency pair JSON object into SymbolInfo (spot).
    ///
    /// Gate.io format:
    /// {
    ///   "id": "BTC_USDT",
    ///   "base": "BTC",
    ///   "quote": "USDT",
    ///   "fee": "0.2",
    ///   "min_base_amount": "0.0001",
    ///   "min_quote_amount": "1",
    ///   "amount_precision": 4,
    ///   "precision": 2,
    ///   "trade_status": "tradable"
    /// }
    [[nodiscard]] static std::optional<SymbolInfo> parse_currency_pair(const nlohmann::json &obj);

    /// Parse a single futures contract JSON object into SymbolInfo.
    ///
    /// Gate.io format:
    /// {
    ///   "name": "BTC_USDT",
    ///   "quanto_multiplier": "0.0001",
    ///   "leverage_min": "1",
    ///   "leverage_max": "125",
    ///   "maintenance_rate": "0.005",
    ///   "funding_interval": 28800,
    ///   "order_size_min": 1,
    ///   "order_size_max": 1000000,
    ///   "order_price_round": "0.1",
    ///   "mark_price": "50000.5",
    ///   "funding_rate": "0.0001"
    /// }
    [[nodiscard]] static std::optional<SymbolInfo> parse_futures_contract(const nlohmann::json &obj);
};

} // namespace pulse::market
