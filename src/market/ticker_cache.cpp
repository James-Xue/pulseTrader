// tickerCache.cpp — TickerCache implementation (Layer 3 Market Data)

#include "market/ticker_cache.hpp"

#include <mutex>

namespace pulse::market
{

void TickerCache::update(const Symbol &symbol, const Ticker &ticker)
{
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_cache[symbol] = ticker;
}

std::optional<Ticker> TickerCache::get(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    const auto it = m_cache.find(symbol);
    if (it == m_cache.end())
    {
        return std::nullopt;
    }
    return it->second;
}

bool TickerCache::contains(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return m_cache.find(symbol) != m_cache.end();
}

std::size_t TickerCache::size() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return m_cache.size();
}

std::vector<Symbol> TickerCache::symbols() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    std::vector<Symbol> result;
    result.reserve(m_cache.size());
    for (const auto &[sym, _] : m_cache)
    {
        result.push_back(sym);
    }
    return result;
}

} // namespace pulse::market
