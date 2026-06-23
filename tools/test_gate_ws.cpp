// test_gate_ws.cpp — Smoke test for Gate.io WebSocket client (Layer 1 Exchange)
//
// Connects to Gate.io's live WebSocket endpoint, subscribes to spot.tickers,
// prints a few updates, then disconnects. Optionally tests private channels
// if GATE_API_KEY and GATE_API_SECRET environment variables are set.
//
// Usage:
//   ./test_gate_ws                         # public channels only
//   GATE_API_KEY=xxx GATE_API_SECRET=yyy ./test_gate_ws  # also tests private channels
//
// NOT part of CTest — this is a manual verification tool.

#include "core/config.hpp"
#include "exchange/gate_ws_client.hpp"
#include "logging/logger.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace pulse;
using namespace pulse::exchange;
using pulse::logging::Logger;

/// Print a JSON value in a human-readable one-line format.
void print_ticker(const std::string &label, const nlohmann::json &result)
{
    const auto pair = result.value("currency_pair", "?");
    const auto last = result.value("last", "?");
    const auto change = result.value("change_percentage", "?");
    const auto vol = result.value("base_volume", "?");
    std::cout << "[" << label << "] " << pair << " last=" << last << " chg=" << change << "% vol=" << vol << std::endl;
}

int main()
{
    // 1. Initialise logging (console only, debug level)
    LogConfig log_cfg;
    log_cfg.level = "debug";
    log_cfg.toConsole = true;
    log_cfg.toFile = false;
    Logger::init(log_cfg);

    // 2. Build exchange config
    ExchangeConfig exchange_cfg;
    exchange_cfg.wsUrl = "wss://api.gateio.ws/ws/v4/";

    // Check for API credentials (optional — enables private channel test)
    const char *env_key = std::getenv("GATE_API_KEY");
    const char *env_secret = std::getenv("GATE_API_SECRET");
    if (nullptr != env_key && nullptr != env_secret)
    {
        exchange_cfg.apiKey = env_key;
        exchange_cfg.apiSecret = env_secret;
        std::cout << "[INFO] API credentials found — will test private channels" << std::endl;
    }
    else
    {
        std::cout << "[INFO] No API credentials — testing public channels only" << std::endl;
    }

    // 3. Create the WebSocket client
    GateWsClient client(exchange_cfg);
    std::atomic<int> ticker_count{ 0 };
    constexpr int max_tickers = 5;

    // 4. Subscribe to spot.tickers for BTC_USDT and ETH_USDT
    client.subscribe("spot.tickers",
        { "BTC_USDT", "ETH_USDT" },
        [&](const nlohmann::json &result, const nlohmann::json & /*frame*/)
        {
            const int count = ticker_count.fetch_add(1) + 1;
            print_ticker("ticker #" + std::to_string(count), result);
        });

    // 5. If credentials are available, subscribe to private spot.orders
    const bool has_auth = !exchange_cfg.apiKey.empty();
    if (has_auth)
    {
        client.subscribePrivate("spot.orders",
            {},
            [](const nlohmann::json &result, const nlohmann::json & /*frame*/)
            { std::cout << "[order] " << result.dump() << std::endl; });
    }

    // 6. Start the client — connects and subscribes
    std::cout << "[INFO] Starting WebSocket client..." << std::endl;
    client.start();

    // 7. Wait for a few ticker updates (up to 30 seconds)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (ticker_count.load() < max_tickers && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // 8. Report results
    const int received = ticker_count.load();
    if (0 == received)
    {
        std::cout << "[FAIL] No ticker updates received in 30 seconds" << std::endl;
    }
    else
    {
        std::cout << "[OK] Received " << received << " ticker update(s)" << std::endl;
    }

    // 9. Stop the client
    std::cout << "[INFO] Stopping WebSocket client..." << std::endl;
    client.stop();

    // 10. Cleanup
    Logger::shutdown();
    std::cout << "[INFO] Done" << std::endl;

    return (0 == received) ? 1 : 0;
}
