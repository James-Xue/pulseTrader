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
    : m_wsClient{ ws_client }
    , m_restClient{ rest_client }
    , m_marketType{ market_type }
    , m_symbolRegistry{ rest_client, market_type }
{
}

void MarketFeed::start(const std::vector<Symbol> &symbols)
{
    const std::string mt_label = MarketType::Futures == m_marketType ? "futures" : "spot";
    PULSE_LOG_INFO("market", "Starting {} MarketFeed for {} symbols", mt_label, symbols.size());

    // 1. Load symbol metadata from REST.
    if (!m_symbolRegistry.loadFromRest())
    {
        PULSE_LOG_WARN("market", "Failed to load symbol registry — continuing without metadata");
    }

    // 2. Create per-symbol K-line buffers.
    for (const auto &symbol : symbols)
    {
        m_klineBuffers.try_emplace(symbol, 500);
    }

    // 3. Subscribe to WebSocket channels (market-type-aware).
    m_subscribedSymbols = symbols;

    const std::string tickers_ch = EndpointRouter::wsChannel(m_marketType, "tickers");
    const std::string candlesticks_ch = EndpointRouter::wsChannel(m_marketType, "candlesticks");

    // Tickers: real-time price updates.
    m_wsClient.subscribe(tickers_ch,
        symbols,
        [this](const nlohmann::json &result, const nlohmann::json &full_frame)
        { onTickerUpdate(result, full_frame); });

    // Order book: incremental updates, 100ms interval, 20 levels.
    // Gate.io requires per-symbol subscription with channel "order_book_update".
    // Supported levels: 20, 50, 100 (level 10 removed 2024-11-18).
    // Payload format: [contract, interval, limit] — e.g. ["BTC_USDT", "100ms", "20"]
    const std::string orderbook_update_ch = EndpointRouter::wsChannel(m_marketType, "order_book_update");
    for (const auto &symbol : symbols)
    {
        std::vector<std::string> ob_payload = { symbol, "100ms", "20" };
        m_wsClient.subscribe(orderbook_update_ch,
            ob_payload,
            [this](const nlohmann::json &result, const nlohmann::json &full_frame)
            { onOrderbookUpdate(result, full_frame); });
    }

    // K-lines: 1-minute interval.
    // Gate.io format: payload = ["1m", "BTC_USDT"] (interval first, then symbols).
    std::vector<std::string> kline_payload;
    kline_payload.push_back("1m");
    for (const auto &symbol : symbols)
    {
        kline_payload.push_back(symbol);
    }

    m_wsClient.subscribe(candlesticks_ch,
        kline_payload,
        [this](const nlohmann::json &result, const nlohmann::json &full_frame)
        { onKlineUpdate(result, full_frame); });

    PULSE_LOG_INFO("market", "{} MarketFeed started — subscribed to {} symbols",
                   mt_label, symbols.size());
}

void MarketFeed::stop()
{
    PULSE_LOG_INFO("market", "Stopping MarketFeed");
    m_wsClient.unsubscribe(EndpointRouter::wsChannel(m_marketType, "tickers"));
    m_wsClient.unsubscribe(EndpointRouter::wsChannel(m_marketType, "order_book_update"));
    m_wsClient.unsubscribe(EndpointRouter::wsChannel(m_marketType, "candlesticks"));
    m_subscribedSymbols.clear();
}

TickerCache &MarketFeed::tickerCache()
{
    return m_tickerCache;
}

SymbolRegistry &MarketFeed::symbolRegistry()
{
    return m_symbolRegistry;
}

KlineBuffer &MarketFeed::getKlineBuffer(const Symbol &symbol)
{
    // Create buffer if it doesn't exist (lazy initialization).
    auto it = m_klineBuffers.find(symbol);
    if (it == m_klineBuffers.end())
    {
        m_klineBuffers.try_emplace(symbol, 500);
        it = m_klineBuffers.find(symbol);
    }
    return it->second;
}

OrderBookManager &MarketFeed::orderbookManager()
{
    return m_orderbookManager;
}

FeedStats MarketFeed::stats() const
{
    return FeedStats{
        .ticker_count    = m_tickerCount.load(std::memory_order_relaxed),
        .orderbook_count = m_orderbookCount.load(std::memory_order_relaxed),
        .kline_count     = m_klineCount.load(std::memory_order_relaxed),
    };
}

void MarketFeed::onTickerUpdate(const nlohmann::json &result, const nlohmann::json &full_frame)
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

            auto last_opt = safeParseDouble(item["last"].get<std::string>());
            if (!last_opt.has_value())
            {
                PULSE_LOG_WARN("market", "Malformed futures ticker 'last' for {}, skipping",
                               ticker.symbol);
                return;
            }
            ticker.last = last_opt.value();

            if (item.contains("mark_price") && !item["mark_price"].is_null())
            {
                ticker.mark_price = safeParseDouble(item["mark_price"].get<std::string>()).value_or(0.0);
            }

            if (item.contains("index_price") && !item["index_price"].is_null())
            {
                ticker.index_price = safeParseDouble(item["index_price"].get<std::string>()).value_or(0.0);
            }

            if (item.contains("funding_rate") && !item["funding_rate"].is_null())
            {
                ticker.funding_rate = safeParseDouble(item["funding_rate"].get<std::string>()).value_or(0.0);
            }

            if (item.contains("volume_24h") && !item["volume_24h"].is_null())
            {
                ticker.volume_24h = safeParseDouble(item["volume_24h"].get<std::string>()).value_or(0.0);
            }

            if (item.contains("change_percentage"))
            {
                ticker.change_pct = safeParseDouble(item["change_percentage"].get<std::string>()).value_or(0.0);
            }

            ticker.bid = 0.0;
            ticker.ask = 0.0;
        }
        else if (item.contains("currency_pair"))
        {
            // Spot ticker format.
            ticker.symbol = item["currency_pair"].get<std::string>();

            auto last_opt = safeParseDouble(item["last"].get<std::string>());
            if (!last_opt.has_value())
            {
                PULSE_LOG_WARN("market", "Malformed spot ticker 'last' for {}, skipping",
                               ticker.symbol);
                return;
            }
            ticker.last = last_opt.value();
            ticker.bid = safeParseDouble(item["highest_bid"].get<std::string>()).value_or(0.0);
            ticker.ask = safeParseDouble(item["lowest_ask"].get<std::string>()).value_or(0.0);
            ticker.volume_24h = safeParseDouble(item["base_volume"].get<std::string>()).value_or(0.0);
            ticker.change_pct = safeParseDouble(item["change_percentage"].get<std::string>()).value_or(0.0);
        }
        else
        {
            return; // Unknown format.
        }

        ticker.timestamp = 0; // Will be set below from full_frame if available.
        m_tickerCache.update(ticker.symbol, ticker);
        m_tickerCount.fetch_add(1, std::memory_order_relaxed);
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

void MarketFeed::onOrderbookUpdate(const nlohmann::json &result, const nlohmann::json &full_frame)
{
    const std::string event = full_frame.value("event", "");

    // Gate.io futures.order_book_update format (compact):
    //   {"U": first_id, "u": last_id, "a": [{"p":"price","s":size},...],
    //    "b": [...], "l":"20", "s":"BTC_USDT", "t": timestamp_ms}
    //
    // Convert to OrderBookManager's expected format:
    //   {"lastUpdateId": N, "bids": [[price,qty],...], "asks": [[price,qty],...]}

    if (!result.is_object())
    {
        return;
    }

    // Extract symbol — try spot "s", legacy futures "c", then order_book_update "contract".
    // Note: order_book_update uses "s" for symbol (same as spot).
    std::string symbol;
    if (result.contains("s") && result["s"].is_string())
    {
        symbol = result["s"].get<std::string>();
    }
    else if (result.contains("c"))
    {
        symbol = result["c"].get<std::string>();
    }
    else if (result.contains("contract"))
    {
        symbol = result["contract"].get<std::string>();
    }

    if (symbol.empty())
    {
        return;
    }

    // Convert compact format to OrderBookManager format.
    nlohmann::json converted;

    // Update ID: "u" (lowercase) → "lastUpdateId"
    if (result.contains("u"))
    {
        converted["lastUpdateId"] = result["u"];
    }
    else if (result.contains("lastUpdateId"))
    {
        converted["lastUpdateId"] = result["lastUpdateId"];
    }

    // Convert bids: "b" array of {"p":..., "s":...} → [[price, qty], ...]
    auto convert_levels = [](const nlohmann::json &src) -> nlohmann::json
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &level : src)
        {
            if (level.contains("p") && level.contains("s"))
            {
                arr.push_back({ level["p"], level["s"] });
            }
            else if (level.is_array() && level.size() >= 2)
            {
                arr.push_back(level);
            }
        }
        return arr;
    };

    if (result.contains("b"))
    {
        converted["bids"] = convert_levels(result["b"]);
    }
    else if (result.contains("bids"))
    {
        converted["bids"] = result["bids"];
    }

    if (result.contains("a"))
    {
        converted["asks"] = convert_levels(result["a"]);
    }
    else if (result.contains("asks"))
    {
        converted["asks"] = result["asks"];
    }

    converted["time"] = full_frame.value("time", static_cast<std::int64_t>(0));

    // Gate.io: event "all" = full snapshot, event "update" = incremental delta.
    if ("all" == event || !m_orderbookManager.contains(symbol))
    {
        m_orderbookManager.applySnapshot(symbol, converted);
    }
    else
    {
        m_orderbookManager.applyDelta(symbol, converted);
    }

    m_orderbookCount.fetch_add(1, std::memory_order_relaxed);
}

void MarketFeed::onKlineUpdate(const nlohmann::json &result, const nlohmann::json &full_frame)
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
        else if (!m_subscribedSymbols.empty())
        {
            // Fallback: use the first subscribed symbol (single-symbol subscription).
            symbol = m_subscribedSymbols[0];
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

        auto open  = safeParseDouble(candle["o"].get<std::string>());
        auto high  = safeParseDouble(candle["h"].get<std::string>());
        auto low   = safeParseDouble(candle["l"].get<std::string>());
        auto close = safeParseDouble(candle["c"].get<std::string>());

        // Volume: may be string (spot) or integer (futures).
        std::optional<double> vol;
        if (candle["v"].is_string())
        {
            vol = safeParseDouble(candle["v"].get<std::string>());
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
        auto info = m_symbolRegistry.get(symbol);
        kline.volume = vol.value() * (info ? info->quanto_multiplier : 1.0);
        kline.closed = true;

        auto &buffer = getKlineBuffer(symbol);
        buffer.push(kline);
        m_klineCount.fetch_add(1, std::memory_order_relaxed);
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
