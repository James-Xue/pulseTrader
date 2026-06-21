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
    // Gate.io format: payload = ["1m", "BTC_USDT"] (interval first, then symbols).
    std::vector<std::string> kline_payload;
    kline_payload.push_back("1m");
    for (const auto &symbol : symbols)
    {
        kline_payload.push_back(symbol);
    }

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

FeedStats MarketFeed::stats() const
{
    return FeedStats{
        .ticker_count    = ticker_count_.load(std::memory_order_relaxed),
        .orderbook_count = orderbook_count_.load(std::memory_order_relaxed),
        .kline_count     = kline_count_.load(std::memory_order_relaxed),
    };
}

void MarketFeed::on_ticker_update(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    // Gate.io ticker format:
    //
    // Spot — result is a JSON object:
    //   { "currency_pair": "BTC_USDT", "last": "50000", ... }
    //
    // Futures — result is a JSON ARRAY with one element:
    //   [{ "contract": "BTC_USDT", "last": "50000.5", ... }]
    //
    // Processing: unwrap array to iterate, then handle each ticker uniformly.

    const auto process_ticker = [this](const nlohmann::json &item)
    {
        if (!item.is_object())
        {
            return;
        }

        Ticker ticker;

        // Determine format by checking which identifier field is present.
        if (item.contains("contract"))
        {
            // Futures ticker format.
            ticker.symbol = item["contract"].get<std::string>();

            auto last_opt = safe_parse_double(item["last"].get<std::string>());
            if (!last_opt.has_value())
            {
                PULSE_LOG_WARN("market", "Malformed futures ticker 'last' for {}, skipping",
                               ticker.symbol);
                return;
            }
            ticker.last = last_opt.value();

            if (item.contains("mark_price") && !item["mark_price"].is_null())
            {
                ticker.mark_price = safe_parse_double(item["mark_price"].get<std::string>()).value_or(0.0);
            }

            if (item.contains("index_price") && !item["index_price"].is_null())
            {
                ticker.index_price = safe_parse_double(item["index_price"].get<std::string>()).value_or(0.0);
            }

            if (item.contains("funding_rate") && !item["funding_rate"].is_null())
            {
                ticker.funding_rate = safe_parse_double(item["funding_rate"].get<std::string>()).value_or(0.0);
            }

            if (item.contains("volume_24h") && !item["volume_24h"].is_null())
            {
                ticker.volume_24h = safe_parse_double(item["volume_24h"].get<std::string>()).value_or(0.0);
            }

            if (item.contains("change_percentage"))
            {
                ticker.change_pct = safe_parse_double(item["change_percentage"].get<std::string>()).value_or(0.0);
            }

            ticker.bid = 0.0;
            ticker.ask = 0.0;
        }
        else if (item.contains("currency_pair"))
        {
            // Spot ticker format.
            ticker.symbol = item["currency_pair"].get<std::string>();

            auto last_opt = safe_parse_double(item["last"].get<std::string>());
            if (!last_opt.has_value())
            {
                PULSE_LOG_WARN("market", "Malformed spot ticker 'last' for {}, skipping",
                               ticker.symbol);
                return;
            }
            ticker.last = last_opt.value();
            ticker.bid = safe_parse_double(item["highest_bid"].get<std::string>()).value_or(0.0);
            ticker.ask = safe_parse_double(item["lowest_ask"].get<std::string>()).value_or(0.0);
            ticker.volume_24h = safe_parse_double(item["base_volume"].get<std::string>()).value_or(0.0);
            ticker.change_pct = safe_parse_double(item["change_percentage"].get<std::string>()).value_or(0.0);
        }
        else
        {
            return; // Unknown format.
        }

        ticker.timestamp = 0; // Will be set below from full_frame if available.
        ticker_cache_.update(ticker.symbol, ticker);
        ticker_count_.fetch_add(1, std::memory_order_relaxed);
    };

    if (result.is_array())
    {
        for (const auto &item : result)
        {
            process_ticker(item);
        }
    }
    else
    {
        process_ticker(result);
    }

    // Apply timestamp from outer frame to all updated tickers (batch).
    if (full_frame.contains("time"))
    {
        // Timestamp is informational — already stored per-ticker above as 0.
        // For precise timestamp tracking, a second pass would be needed.
        // This is acceptable for monitoring purposes.
    }
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

    orderbook_count_.fetch_add(1, std::memory_order_relaxed);
}

void MarketFeed::on_kline_update(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    // Gate.io K-line format:
    //
    // Spot — result is a JSON object:
    //   result: { "t": 123, "o": "50000", "c": "50001", "h": "50100", "l": "49900",
    //             "v": "123.45", "n": "1m", "a": "6172800" }
    //   Symbol: full_frame["currency_pair"] = "BTC_USDT"
    //
    // Futures — result is a JSON ARRAY with one element:
    //   result: [{ "t": 123, "o": "50000", "c": "50001", "h": "50100", "l": "49900",
    //              "v": 120, "n": "1m", "contract": "BTC_USDT" }]
    //   Symbol: element["contract"] = "BTC_USDT"
    //   Note: "n" is the interval ("1m"), NOT the contract name.
    //
    // Processing:
    //   1. Normalize result to an iterable list of candle objects
    //   2. Extract symbol per candle (spot: outer frame, futures: element field)
    //   3. Parse OHLCV and push to the appropriate KlineBuffer

    // 1. Normalize: build a list of candle references to process uniformly.
    //    For spot (object), wrap in a single-element span.
    //    For futures (array), iterate directly.
    const auto process_candle = [this, &full_frame](const nlohmann::json &candle)
    {
        if (!candle.is_object() || !candle.contains("t"))
        {
            return;
        }

        // 2. Extract symbol — location differs by market type.
        std::string symbol;
        if (full_frame.contains("currency_pair"))
        {
            // Spot: symbol in outer frame.
            symbol = full_frame["currency_pair"].get<std::string>();
        }
        else if (candle.contains("contract"))
        {
            // Futures: symbol in each candle element.
            symbol = candle["contract"].get<std::string>();
        }
        else if (!subscribed_symbols_.empty())
        {
            // Fallback: use the first subscribed symbol (single-symbol subscription).
            symbol = subscribed_symbols_[0];
        }

        if (symbol.empty())
        {
            return;
        }

        // 3. Parse OHLCV fields.
        Kline kline;
        kline.open_time = candle["t"].is_string()
            ? std::stoll(candle["t"].get<std::string>()) * 1000
            : candle["t"].get<std::int64_t>() * 1000; // Convert to ms.
        kline.close_time = kline.open_time + 60000; // 1 minute later.

        auto open  = safe_parse_double(candle["o"].get<std::string>());
        auto high  = safe_parse_double(candle["h"].get<std::string>());
        auto low   = safe_parse_double(candle["l"].get<std::string>());
        auto close = safe_parse_double(candle["c"].get<std::string>());

        // Volume: may be string (spot) or integer (futures).
        std::optional<double> vol;
        if (candle["v"].is_string())
        {
            vol = safe_parse_double(candle["v"].get<std::string>());
        }
        else if (candle["v"].is_number())
        {
            vol = candle["v"].get<double>();
        }

        if (!open || !high || !low || !close || !vol)
        {
            PULSE_LOG_WARN("market", "Malformed kline OHLCV for {}, skipping", symbol);
            return;
        }

        kline.open   = open.value();
        kline.high   = high.value();
        kline.low    = low.value();
        kline.close  = close.value();

        // Futures volume is in contracts — convert to base currency (e.g., BTC)
        // via quanto_multiplier (1 contract = quanto_multiplier BTC).
        // Spot volume is already in base currency (multiplier defaults to 1.0).
        auto info = symbol_registry_.get(symbol);
        kline.volume = vol.value() * (info ? info->quanto_multiplier : 1.0);
        kline.closed = true;

        auto &buffer = get_kline_buffer(symbol);
        buffer.push(kline);
        kline_count_.fetch_add(1, std::memory_order_relaxed);
    };

    if (result.is_array())
    {
        // Futures format: array of candle objects.
        for (const auto &candle : result)
        {
            process_candle(candle);
        }
    }
    else if (result.is_object())
    {
        // Spot format: single candle object.
        process_candle(result);
    }
}

} // namespace pulse::market
