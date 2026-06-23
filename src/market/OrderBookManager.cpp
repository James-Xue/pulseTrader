// orderbookManager.cpp — OrderBookManager implementation (Layer 3 Market Data)

#include "market/OrderBookManager.hpp"

#include "logging/Logger.hpp"

namespace pulse::market
{

using namespace pulse::logging;

void OrderBookManager::setResubscribeCallback(ResubscribeCallback callback)
{
    m_resubscribeCallback = std::move(callback);
}

void OrderBookManager::applySnapshot(const Symbol &symbol, const nlohmann::json &snapshot)
{
    // Validate JSON structure.
    if (!snapshot.contains("lastUpdateId") || !snapshot.contains("bids") || !snapshot.contains("asks"))
    {
        PULSE_LOG_WARN("market", "Invalid snapshot format for {}", symbol);
        return;
    }

    OrderBook book;
    book.symbol = symbol;
    book.sequence_id = snapshot["lastUpdateId"].is_string()
        ? std::stoull(snapshot["lastUpdateId"].get<std::string>())
        : snapshot["lastUpdateId"].get<std::uint64_t>();
    book.timestamp = snapshot.value("time", static_cast<std::int64_t>(0));

    // Parse bids and asks.
    parseLevels(book.bids, snapshot["bids"]);
    parseLevels(book.asks, snapshot["asks"]);

    // Store the book and update sequence tracker.
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_books[symbol] = std::move(book);
    m_lastSequence[symbol] = m_books[symbol].sequence_id;

    PULSE_LOG_DEBUG("market",
        "Snapshot applied for {}: {} bids, {} asks, seq={}",
        symbol,
        m_books[symbol].bids.size(),
        m_books[symbol].asks.size(),
        m_books[symbol].sequence_id);
}

void OrderBookManager::applyDelta(const Symbol &symbol, const nlohmann::json &delta)
{
    // Validate JSON structure.
    if (!delta.contains("lastUpdateId") || !delta.contains("bids") || !delta.contains("asks"))
    {
        PULSE_LOG_WARN("market", "Invalid delta format for {}", symbol);
        return;
    }

    const std::uint64_t delta_seq = delta["lastUpdateId"].is_string()
        ? std::stoull(delta["lastUpdateId"].get<std::string>())
        : delta["lastUpdateId"].get<std::uint64_t>();

    std::unique_lock<std::shared_mutex> write_lock(m_mutex);

    // Check if we have a snapshot for this symbol.
    const auto book_it = m_books.find(symbol);
    if (book_it == m_books.end())
    {
        PULSE_LOG_WARN("market", "Delta received for {} before snapshot — ignoring", symbol);
        return;
    }

    // Validate sequence number.
    // Gate.io's lastUpdateId is a GLOBAL counter shared across all symbols'
    // order book updates, NOT a per-symbol sequential ID.  Gaps between
    // consecutive deltas for the same symbol are normal (other symbols'
    // updates increment the global counter in between).  We only reject
    // stale or duplicate deltas (delta_seq <= last_seq).
    const auto seq_it = m_lastSequence.find(symbol);
    const std::uint64_t last_seq = (seq_it != m_lastSequence.end()) ? seq_it->second : 0;

    if (delta_seq <= last_seq && 0 != last_seq)
    {
        PULSE_LOG_DEBUG("market",
            "Stale delta for {}: seq {} <= last {}, ignoring",
            symbol, delta_seq, last_seq);
        return;
    }

    // Apply delta updates.
    applyDeltaLevels(book_it->second.bids, delta["bids"]);
    applyDeltaLevels(book_it->second.asks, delta["asks"]);

    // Update sequence ID and timestamp.
    book_it->second.sequence_id = delta_seq;
    book_it->second.timestamp = delta.value("time", book_it->second.timestamp);
    m_lastSequence[symbol] = delta_seq;
}

std::optional<OrderBook> OrderBookManager::get(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    const auto it = m_books.find(symbol);
    if (it == m_books.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::vector<OrderBookLevel> OrderBookManager::topBids(const Symbol &symbol, std::size_t n) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    const auto it = m_books.find(symbol);
    if (it == m_books.end())
    {
        return {};
    }

    std::vector<OrderBookLevel> result;
    result.reserve(n);

    std::size_t count = 0;
    for (const auto &[price, qty] : it->second.bids)
    {
        if (count >= n || 0.0 == qty)
        {
            break;
        }
        result.push_back({ price, qty });
        ++count;
    }

    return result;
}

std::vector<OrderBookLevel> OrderBookManager::topAsks(const Symbol &symbol, std::size_t n) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    const auto it = m_books.find(symbol);
    if (it == m_books.end())
    {
        return {};
    }

    std::vector<OrderBookLevel> result;
    result.reserve(n);

    std::size_t count = 0;
    for (const auto &[price, qty] : it->second.asks)
    {
        if (count >= n || 0.0 == qty)
        {
            break;
        }
        result.push_back({ price, qty });
        ++count;
    }

    return result;
}

bool OrderBookManager::contains(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return m_books.find(symbol) != m_books.end();
}

template <typename Compare>
void OrderBookManager::parseLevels(std::map<Price, Quantity, Compare> &out, const nlohmann::json &levels_json)
{
    if (!levels_json.is_array())
    {
        return;
    }

    for (const auto &level : levels_json)
    {
        if (!level.is_array() || level.size() < 2)
        {
            continue;
        }

        Price price;
        Quantity qty;

        if (level[0].is_string())
        {
            auto opt = safeParseDouble(level[0].get<std::string>());
            if (!opt.has_value()) { continue; }
            price = opt.value();
        }
        else
        {
            price = level[0].get<Price>();
        }

        if (level[1].is_string())
        {
            auto opt = safeParseDouble(level[1].get<std::string>());
            if (!opt.has_value()) { continue; }
            qty = opt.value();
        }
        else
        {
            qty = level[1].get<Quantity>();
        }

        if (qty > 0.0)
        {
            out[price] = qty;
        }
    }
}

template <typename Compare>
void OrderBookManager::applyDeltaLevels(std::map<Price, Quantity, Compare> &book, const nlohmann::json &levels_json)
{
    if (!levels_json.is_array())
    {
        return;
    }

    for (const auto &level : levels_json)
    {
        if (!level.is_array() || level.size() < 2)
        {
            continue;
        }

        Price price;
        Quantity qty;

        if (level[0].is_string())
        {
            auto opt = safeParseDouble(level[0].get<std::string>());
            if (!opt.has_value()) { continue; }
            price = opt.value();
        }
        else
        {
            price = level[0].get<Price>();
        }

        if (level[1].is_string())
        {
            auto opt = safeParseDouble(level[1].get<std::string>());
            if (!opt.has_value()) { continue; }
            qty = opt.value();
        }
        else
        {
            qty = level[1].get<Quantity>();
        }

        if (0.0 == qty)
        {
            // Remove the level.
            book.erase(price);
        }
        else
        {
            // Insert or update the level.
            book[price] = qty;
        }
    }
}

// Explicit template instantiations.
template void OrderBookManager::parseLevels<std::greater<Price>>(
    std::map<Price, Quantity, std::greater<Price>> &, const nlohmann::json &);
template void OrderBookManager::parseLevels<std::less<Price>>(
    std::map<Price, Quantity, std::less<Price>> &, const nlohmann::json &);
template void OrderBookManager::applyDeltaLevels<std::greater<Price>>(
    std::map<Price, Quantity, std::greater<Price>> &, const nlohmann::json &);
template void OrderBookManager::applyDeltaLevels<std::less<Price>>(
    std::map<Price, Quantity, std::less<Price>> &, const nlohmann::json &);

} // namespace pulse::market
