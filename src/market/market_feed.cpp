// market_feed.cpp — MarketFeed dispatcher implementation (Layer 3 Market Data)

#include "pulse/market/market_feed.hpp"

#include "pulse/logging/logger.hpp"

namespace pulse::market
{

using namespace pulse::logging;

MarketFeed::MarketFeed(exchange::GateWsClient &ws_client, exchange::GateRestClient &rest_client)
    : ws_client_{ ws_client }
    , rest_client_{ rest_client }
    , symbol_registry_{ rest_client }
{
}

void MarketFeed::start(const std::vector<Symbol> &symbols)
{
    PULSE_LOG_INFO("market", "Starting MarketFeed for {} symbols", symbols.size());

    // 1. Load symbol metadata from REST.
    if (!symbol_registry_.load_from_rest())
    {
        PULSE_LOG_WARN("market", "Failed to load symbol registry — continuing without metadata");
    }

    // 2. Create per-symbol K-line buffers.
    for (const auto &symbol : symbols)
    {
        kline_buffers_.try_emplace(symbol, 500);
    }

    // 3. Subscribe to WebSocket channels.
    subscribed_symbols_ = symbols;

    // Tickers: real-time price updates.
    ws_client_.subscribe("spot.tickers",
        symbols,
        [this](const nlohmann::json &result, const nlohmann::json &full_frame)
        { on_ticker_update(result, full_frame); });

    // Order book: 10 levels, 100ms update interval.
    // Gate.io format: payload = ["BTC_USDT", "10", "100ms"]
    std::vector<std::string> orderbook_payload;
    for (const auto &symbol : symbols)
    {
        orderbook_payload.push_back(symbol);
    }
    orderbook_payload.push_back("10");
    orderbook_payload.push_back("100ms");

    ws_client_.subscribe("spot.order_book",
        orderbook_payload,
        [this](const nlohmann::json &result, const nlohmann::json &full_frame)
        { on_orderbook_update(result, full_frame); });

    // K-lines: 1-minute interval.
    // Gate.io format: payload = ["BTC_USDT", "1m"]
    std::vector<std::string> kline_payload;
    for (const auto &symbol : symbols)
    {
        kline_payload.push_back(symbol);
    }
    kline_payload.push_back("1m");

    ws_client_.subscribe("spot.candlesticks",
        kline_payload,
        [this](const nlohmann::json &result, const nlohmann::json &full_frame)
        { on_kline_update(result, full_frame); });

    PULSE_LOG_INFO("market", "MarketFeed started — subscribed to {} symbols", symbols.size());
}

void MarketFeed::stop()
{
    PULSE_LOG_INFO("market", "Stopping MarketFeed");
    ws_client_.unsubscribe("spot.tickers");
    ws_client_.unsubscribe("spot.order_book");
    ws_client_.unsubscribe("spot.candlesticks");
    subscribed_symbols_.clear();
}

TickerCache &MarketFeed::ticker_cache()
{
    return ticker_cache_;
}

SymbolRegistry &MarketFeed::symbol_registry()
{
    return symbol_registry_;
}

KlineBuffer &MarketFeed::get_kline_buffer(const Symbol &symbol)
{
    // Create buffer if it doesn't exist (lazy initialization).
    auto it = kline_buffers_.find(symbol);
    if (it == kline_buffers_.end())
    {
        kline_buffers_.try_emplace(symbol, 500);
        it = kline_buffers_.find(symbol);
    }
    return it->second;
}

OrderBookManager &MarketFeed::orderbook_manager()
{
    return orderbook_manager_;
}

void MarketFeed::on_ticker_update(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    // Gate.io ticker format:
    // {
    //   "currency_pair": "BTC_USDT",
    //   "last": "50000",
    //   "lowest_ask": "50001",
    //   "highest_bid": "49999",
    //   "change_percentage": "2.5",
    //   "base_volume": "1234.56",
    //   "quote_volume": "61728000",
    //   "high_24h": "51000",
    //   "low_24h": "49000"
    // }

    if (!result.is_object() || !result.contains("currency_pair"))
    {
        return;
    }

    Ticker ticker;
    ticker.symbol = result["currency_pair"].get<std::string>();
    ticker.last = std::stod(result["last"].get<std::string>());
    ticker.bid = std::stod(result["highest_bid"].get<std::string>());
    ticker.ask = std::stod(result["lowest_ask"].get<std::string>());
    ticker.volume_24h = std::stod(result["base_volume"].get<std::string>());
    ticker.change_pct = std::stod(result["change_percentage"].get<std::string>());
    ticker.timestamp = full_frame.value("time", static_cast<std::int64_t>(0));

    ticker_cache_.update(ticker.symbol, ticker);
}

void MarketFeed::on_orderbook_update(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    // Gate.io order book format:
    // Snapshot: {"lastUpdateId": 123, "bids": [[price, qty], ...], "asks": [[price, qty], ...]}
    // Delta: same format with incrementing lastUpdateId

    if (!result.is_object())
    {
        return;
    }

    // Gate.io order_book puts the symbol in result["s"], not in the outer frame.
    const std::string symbol = result.value("s", "");
    if (symbol.empty())
    {
        return;
    }

    // Gate.io: event "all" = full snapshot, event "update" = incremental delta.
    const std::string event = full_frame.value("event", "");

    if ("all" == event || !orderbook_manager_.contains(symbol))
    {
        // Snapshot — replace the entire book.
        nlohmann::json snapshot = result;
        snapshot["time"] = full_frame.value("time", static_cast<std::int64_t>(0));
        orderbook_manager_.apply_snapshot(symbol, snapshot);
    }
    else
    {
        // Incremental delta update.
        nlohmann::json delta = result;
        delta["time"] = full_frame.value("time", static_cast<std::int64_t>(0));
        orderbook_manager_.apply_delta(symbol, delta);
    }
}

void MarketFeed::on_kline_update(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    // Gate.io K-line format:
    // {
    //   "t": 1234567890,  // open time (Unix seconds)
    //   "v": "123.45",    // volume
    //   "c": "50000",     // close price
    //   "h": "50100",     // high price
    //   "l": "49900",     // low price
    //   "o": "50000",     // open price
    //   "n": "1m",        // interval
    //   "a": "6172800"    // quote volume
    // }

    if (!result.is_object() || !result.contains("t"))
    {
        return;
    }

    const std::string symbol = full_frame.value("currency_pair", "");
    if (symbol.empty())
    {
        return;
    }

    Kline kline;
    kline.open_time = result["t"].get<std::int64_t>() * 1000; // Convert to ms.
    kline.close_time = kline.open_time + 60000;               // 1 minute later.
    kline.open = std::stod(result["o"].get<std::string>());
    kline.high = std::stod(result["h"].get<std::string>());
    kline.low = std::stod(result["l"].get<std::string>());
    kline.close = std::stod(result["c"].get<std::string>());
    kline.volume = std::stod(result["v"].get<std::string>());
    kline.closed = true; // Assume closed for simplicity (could check if current candle).

    auto &buffer = get_kline_buffer(symbol);
    buffer.push(kline);
}

} // namespace pulse::market
