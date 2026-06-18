// test_market_feed.cpp — Smoke test for Layer 3 Market Data Pipeline
//
// Connects to Gate.io WebSocket, subscribes to BTC_USDT channels (ticker, order book, K-lines),
// and prints real-time updates for 30 seconds.
//
// Usage:
//   ./test_market_feed
//
// NOT part of CTest — this is a manual verification tool.

#include "core/config.hpp"
#include "exchange/gate_rest_client.hpp"
#include "exchange/gate_ws_client.hpp"
#include "logging/logger.hpp"
#include "market/market_feed.hpp"

#include <chrono>
#include <iostream>
#include <thread>

using namespace pulse;
using namespace pulse::exchange;
using namespace pulse::market;
using namespace pulse::logging;

/// Print a ticker update.
void print_ticker(const Ticker &ticker)
{
    std::cout << "[TICKER] " << ticker.symbol << " last=" << ticker.last << " bid=" << ticker.bid
              << " ask=" << ticker.ask << " vol=" << ticker.volume_24h << " chg=" << ticker.change_pct << "%"
              << std::endl;
}

/// Print order book top 5 levels.
void print_orderbook(const OrderBookManager &manager, const Symbol &symbol)
{
    const auto top_bids = manager.top_bids(symbol, 5);
    const auto top_asks = manager.top_asks(symbol, 5);

    std::cout << "[ORDERBOOK] " << symbol << std::endl;

    // Print asks (reversed so highest is on top).
    for (auto it = top_asks.rbegin(); it != top_asks.rend(); ++it)
    {
        std::cout << "  ASK " << it->price << " x " << it->quantity << std::endl;
    }

    std::cout << "  ---" << std::endl;

    // Print bids.
    for (const auto &level : top_bids)
    {
        std::cout << "  BID " << level.price << " x " << level.quantity << std::endl;
    }
}

/// Print latest K-line.
void print_kline(const KlineBuffer &buffer, const Symbol &symbol)
{
    const auto latest = buffer.latest();
    if (!latest.has_value())
    {
        return;
    }

    const auto &k = *latest;
    std::cout << "[KLINE] " << symbol << " O=" << k.open << " H=" << k.high << " L=" << k.low << " C=" << k.close
              << " V=" << k.volume << std::endl;
}

int main()
{
    // 1. Initialise logging (console only, debug level).
    LogConfig log_cfg;
    log_cfg.level = "debug";
    log_cfg.toConsole = true;
    log_cfg.toFile = false;
    Logger::init(log_cfg);

    // 2. Build exchange config.
    ExchangeConfig exchange_cfg;
    exchange_cfg.wsUrl = "wss://api.gateio.ws/ws/v4/";

    // 3. Create REST and WS clients.
    GateRestClient rest_client(exchange_cfg);
    GateWsClient ws_client(exchange_cfg);

    // 4. Create MarketFeed dispatcher.
    MarketFeed feed(ws_client, rest_client);

    // 5. Start the WebSocket client.
    std::cout << "[INFO] Starting WebSocket client..." << std::endl;
    ws_client.start();

    // 6. Start MarketFeed (subscribes to channels).
    std::cout << "[INFO] Starting MarketFeed for BTC_USDT..." << std::endl;
    feed.start({ "BTC_USDT" });

    // 7. Print updates for 30 seconds.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int print_count = 0;
    constexpr int max_prints = 20;

    while (std::chrono::steady_clock::now() < deadline && print_count < max_prints)
    {
        // Print ticker.
        const auto ticker = feed.ticker_cache().get("BTC_USDT");
        if (ticker.has_value())
        {
            print_ticker(*ticker);
            ++print_count;
        }

        // Print order book (every 3rd iteration to avoid flooding).
        if (print_count % 3 == 0)
        {
            print_orderbook(feed.orderbook_manager(), "BTC_USDT");
        }

        // Print K-line (every 5th iteration).
        if (print_count % 5 == 0)
        {
            print_kline(feed.get_kline_buffer("BTC_USDT"), "BTC_USDT");
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 8. Report results.
    if (0 == print_count)
    {
        std::cout << "[FAIL] No market data received in 30 seconds" << std::endl;
    }
    else
    {
        std::cout << "[OK] Received " << print_count << " update(s)" << std::endl;
    }

    // 9. Stop MarketFeed and WS client.
    std::cout << "[INFO] Stopping MarketFeed..." << std::endl;
    feed.stop();

    std::cout << "[INFO] Stopping WebSocket client..." << std::endl;
    ws_client.stop();

    // 10. Cleanup.
    Logger::shutdown();
    std::cout << "[INFO] Done" << std::endl;

    return (0 == print_count) ? 1 : 0;
}
