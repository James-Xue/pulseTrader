// orderbook_manager.cpp — OrderBookManager implementation (Layer 3 Market Data)

#include "market/orderbook_manager.hpp"

#include "logging/logger.hpp"

namespace pulse::market
{

using namespace pulse::logging;

void OrderBookManager::set_resubscribe_callback(ResubscribeCallback callback)
{
    resubscribe_callback_ = std::move(callback);
}

void OrderBookManager::apply_snapshot(const Symbol &symbol, const nlohmann::json &snapshot)
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
    parse_levels(book.bids, snapshot["bids"]);
    parse_levels(book.asks, snapshot["asks"]);

    // Store the book and update sequence tracker.
    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    books_[symbol] = std::move(book);
    last_sequence_[symbol] = books_[symbol].sequence_id;

    PULSE_LOG_DEBUG("market",
        "Snapshot applied for {}: {} bids, {} asks, seq={}",
        symbol,
        books_[symbol].bids.size(),
        books_[symbol].asks.size(),
        books_[symbol].sequence_id);
}

void OrderBookManager::apply_delta(const Symbol &symbol, const nlohmann::json &delta)
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

    std::unique_lock<std::shared_mutex> write_lock(mutex_);

    // Check if we have a snapshot for this symbol.
    const auto book_it = books_.find(symbol);
    if (book_it == books_.end())
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
    const auto seq_it = last_sequence_.find(symbol);
    const std::uint64_t last_seq = (seq_it != last_sequence_.end()) ? seq_it->second : 0;

    if (delta_seq <= last_seq && 0 != last_seq)
    {
        PULSE_LOG_DEBUG("market",
            "Stale delta for {}: seq {} <= last {}, ignoring",
            symbol, delta_seq, last_seq);
        return;
    }

    // Apply delta updates.
    apply_delta_levels(book_it->second.bids, delta["bids"]);
    apply_delta_levels(book_it->second.asks, delta["asks"]);

    // Update sequence ID and timestamp.
    book_it->second.sequence_id = delta_seq;
    book_it->second.timestamp = delta.value("time", book_it->second.timestamp);
    last_sequence_[symbol] = delta_seq;
}

std::optional<OrderBook> OrderBookManager::get(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    const auto it = books_.find(symbol);
    if (it == books_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::vector<OrderBookLevel> OrderBookManager::top_bids(const Symbol &symbol, std::size_t n) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    const auto it = books_.find(symbol);
    if (it == books_.end())
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

std::vector<OrderBookLevel> OrderBookManager::top_asks(const Symbol &symbol, std::size_t n) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    const auto it = books_.find(symbol);
    if (it == books_.end())
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
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    return books_.find(symbol) != books_.end();
}

template <typename Compare>
void OrderBookManager::parse_levels(std::map<Price, Quantity, Compare> &out, const nlohmann::json &levels_json)
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

        const Price price = level[0].is_string()
            ? std::stod(level[0].get<std::string>())
            : level[0].get<Price>();
        const Quantity qty = level[1].is_string()
            ? std::stod(level[1].get<std::string>())
            : level[1].get<Quantity>();

        if (qty > 0.0)
        {
            out[price] = qty;
        }
    }
}

template <typename Compare>
void OrderBookManager::apply_delta_levels(std::map<Price, Quantity, Compare> &book, const nlohmann::json &levels_json)
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

        const Price price = level[0].is_string()
            ? std::stod(level[0].get<std::string>())
            : level[0].get<Price>();
        const Quantity qty = level[1].is_string()
            ? std::stod(level[1].get<std::string>())
            : level[1].get<Quantity>();

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
template void OrderBookManager::parse_levels<std::greater<Price>>(
    std::map<Price, Quantity, std::greater<Price>> &, const nlohmann::json &);
template void OrderBookManager::parse_levels<std::less<Price>>(
    std::map<Price, Quantity, std::less<Price>> &, const nlohmann::json &);
template void OrderBookManager::apply_delta_levels<std::greater<Price>>(
    std::map<Price, Quantity, std::greater<Price>> &, const nlohmann::json &);
template void OrderBookManager::apply_delta_levels<std::less<Price>>(
    std::map<Price, Quantity, std::less<Price>> &, const nlohmann::json &);

} // namespace pulse::market
