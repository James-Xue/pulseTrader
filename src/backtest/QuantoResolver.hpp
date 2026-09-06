#pragma once
// quanto_resolver.hpp — Futures contract metadata for any Gate USDT-M coin (M33)
//
// The M29 backtest hardcoded quanto multipliers for ETH/BTC and silently
// scored every other symbol with 1.0. This resolver auto-fetches the public
// contract list (GET /futures/usdt/contracts), caches it to a JSON file with
// a TTL, and resolves quanto_multiplier + contract limits for ANY symbol —
// or fails fast with a did-you-mean error before any kline fetch happens.

#include "core/PulseError.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace pulse::exchange
{
class GateRestClient;
}

namespace pulse::market
{
struct SymbolInfo;
}

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// QuantoResolver — quanto_multiplier / contract validation, cache-backed
// ---------------------------------------------------------------------------
class QuantoResolver
{
  public:
    /// `cache_path` empty disables the on-disk cache (always fetch).
    QuantoResolver(exchange::GateRestClient &rest, std::string cache_path);

    /// Spot returns 1.0 without touching the network. Futures resolves via
    /// cache → REST. CFD is an error (no kline source for it anyway).
    /// On an unknown futures symbol the error names the symbol, the total
    /// contract count, up to 15 candidate names and the --quanto escape hatch.
    [[nodiscard]] Result<double> resolveQuanto(
        const std::string &symbol, MarketType market_type);

    /// Full futures contract metadata (validation + order sizing hints).
    [[nodiscard]] Result<market::SymbolInfo> futuresContract(
        const std::string &symbol);

  private:
    /// Loads the cache file if fresh, else fetches the whole contract list.
    [[nodiscard]] Result<bool> ensureLoaded();

    /// True when the in-memory cache is present and under the TTL.
    [[nodiscard]] bool cacheFresh() const;

    void loadCacheFromFile();
    void saveCacheToFile();

    exchange::GateRestClient &m_rest;
    std::string m_cachePath;
    std::int64_t m_fetchedAtMs = 0; ///< 0 = never fetched / no cache file.
    std::unordered_map<std::string, market::SymbolInfo> m_contracts;
};

} // namespace pulse::backtest
