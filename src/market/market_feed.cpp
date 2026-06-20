// market_feed.cpp — MarketFeed dispatcher implementation (Layer 3 Market Data)

#include "market/market_feed.hpp"

#include "exchange/endpoint_router.hpp"
#include "logging/logger.hpp"

namespace pulse::market
{

using namespace pulse::logging;
using pulse::exchange::EndpointRouter;

MarketFeed::MarketFeed(exchange::GateWsClient &ws_client, exchange::GateRestClient &rest_client,
                       MarketType market_type)
    : ws_client_{ ws_client }
    , rest_client_{ rest_client }
    , market_type_{ market_type }
    , symbol_registry_{ rest_client, market_type }
{
}

void MarketFeed::start(const std::vector<Symbol> &symbols)
{
    const std::string mt_label = MarketType::Futures == market_type_ ? "futures" : "spot";
    PULSE_LOG_INFO("market", "Starting {} MarketFeed for {} symbols", mt_label, symbols.size());

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

    // 3. Subscribe to WebSocket channels (market-type-aware).
    subscribed_symbols_ = symbols;

    const std::string tickers_ch = EndpointRouter::ws_channel(market_type_, "tickers");
    const std::string orderbook_ch = EndpointRouter::ws_channel(market_type_, "order_book");
    const std::string candlesticks_ch = EndpointRouter::ws_channel(market_type_, "candlesticks");

    // Tickers: real-time price updates.
    ws_client_.subscribe(tickers_ch,
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

    ws_client_.subscribe(orderbook_ch,
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

    ws_client_.subscribe(candlesticks_ch,
        kline_payload,
        [this](const nlohmann::json &result, const nlohmann::json &full_frame)
        { on_kline_update(result, full_frame); });

    PULSE_LOG_INFO("market", "{} MarketFeed started — subscribed to {} symbols",
                   mt_label, symbols.size());
}

void MarketFeed::stop()
{
    PULSE_LOG_INFO("market", "Stopping MarketFeed");
    ws_client_.unsubscribe(EndpointRouter::ws_channel(market_type_, "tickers"));
    ws_client_.unsubscribe(EndpointRouter::ws_channel(market_type_, "order_book"));
    ws_client_.unsubscribe(EndpointRouter::ws_channel(market_type_, "candlesticks"));
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
    // Gate.io spot ticker format:
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
    //
    // Gate.io futures ticker format:
    // {
    //   "contract": "BTC_USDT",
    //   "last": "50000.5",
    //   "change_percentage": "2.5",
    //   "funding_rate": "0.0001",
    //   "mark_price": "50000.5",
    //   "index_price": "50001.0",
    //   "volume_24h": "123456789",
    //   "quanto_multiplier": "0.0001"
    // }

    if (!result.is_object())
    {
        return;
    }

    Ticker ticker;

    // Determine format by checking which identifier field is present.
    if (result.contains("contract"))
    {
        // Futures ticker format.
        ticker.symbol = result["contract"].get<std::string>();
        ticker.last = std::stod(result["last"].get<std::string>());

        if (result.contains("mark_price") && !result["mark_price"].is_null())
        {
            ticker.mark_price = std::stod(result["mark_price"].get<std::string>());
        }

        if (result.contains("index_price") && !result["index_price"].is_null())
        {
            ticker.index_price = std::stod(result["index_price"].get<std::string>());
        }

        if (result.contains("funding_rate") && !result["funding_rate"].is_null())
        {
            ticker.funding_rate = std::stod(result["funding_rate"].get<std::string>());
        }

        // Futures uses volume_24h (in contracts) instead of base_volume.
        if (result.contains("volume_24h") && !result["volume_24h"].is_null())
        {
            ticker.volume_24h = std::stod(result["volume_24h"].get<std::string>());
        }

        if (result.contains("change_percentage"))
        {
            ticker.change_pct = std::stod(result["change_percentage"].get<std::string>());
        }

        // Futures tickers don't have bid/ask in the ticker channel (those come from orderbook).
        ticker.bid = 0.0;
        ticker.ask = 0.0;
    }
    else if (result.contains("currency_pair"))
    {
        // Spot ticker format (unchanged).
        ticker.symbol = result["currency_pair"].get<std::string>();
        ticker.last = std::stod(result["last"].get<std::string>());
        ticker.bid = std::stod(result["highest_bid"].get<std::string>());
        ticker.ask = std::stod(result["lowest_ask"].get<std::string>());
        ticker.volume_24h = std::stod(result["base_volume"].get<std::string>());
        ticker.change_pct = std::stod(result["change_percentage"].get<std::string>());
    }
    else
    {
        return; // Unknown format.
    }

    ticker.timestamp = full_frame.value("time", static_cast<std::int64_t>(0));
    ticker_cache_.update(ticker.symbol, ticker);
}

void MarketFeed::on_orderbook_update(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    // Gate.io order book format:
    // Snapshot: {"lastUpdateId": 123, "bids": [[price, qty], ...], "asks": [[price, qty], ...]}
    // Delta: same format with incrementing lastUpdateId
    //
    // Symbol field:
    //   Spot:    result["s"] = "BTC_USDT"
    //   Futures: result["c"] = "BTC_USDT" (contract name)

    if (!result.is_object())
    {
        return;
    }

    // Extract symbol — try spot "s" first, then futures "c".
    std::string symbol;
    if (result.contains("s"))
    {
        symbol = result["s"].get<std::string>();
    }
    else if (result.contains("c"))
    {
        symbol = result["c"].get<std::string>();
    }

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
    // Gate.io K-line format (same for spot and futures):
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
    //
    // Symbol field in outer frame:
    //   Spot:    full_frame["currency_pair"] = "BTC_USDT"
    //   Futures: full_frame["contract"] = "BTC_USDT"

    if (!result.is_object() || !result.contains("t"))
    {
        return;
    }

    // Extract symbol — try spot "currency_pair" first, then futures "contract".
    std::string symbol;
    if (full_frame.contains("currency_pair"))
    {
        symbol = full_frame["currency_pair"].get<std::string>();
    }
    else if (full_frame.contains("contract"))
    {
        symbol = full_frame["contract"].get<std::string>();
    }

    if (symbol.empty())
    {
        return;
    }

    Kline kline;
    kline.open_time = result["t"].is_string()
        ? std::stoll(result["t"].get<std::string>()) * 1000
        : result["t"].get<std::int64_t>() * 1000; // Convert to ms.
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
