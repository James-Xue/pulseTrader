// ticker_cache.cpp — TickerCache implementation (Layer 3 Market Data)

#include "pulse/market/ticker_cache.hpp"

#include <mutex>

namespace pulse::market
{

void TickerCache::update(const Symbol &symbol, const Ticker &ticker)
{
    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    cache_[symbol] = ticker;
}

std::optional<Ticker> TickerCache::get(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    const auto it = cache_.find(symbol);
    if (it == cache_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

bool TickerCache::contains(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    return cache_.find(symbol) != cache_.end();
}

std::size_t TickerCache::size() const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    return cache_.size();
}

std::vector<Symbol> TickerCache::symbols() const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    std::vector<Symbol> result;
    result.reserve(cache_.size());
    for (const auto &[sym, _] : cache_)
    {
        result.push_back(sym);
    }
    return result;
}

} // namespace pulse::market
