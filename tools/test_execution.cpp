// test_execution.cpp — Smoke test for Layer 8 Order Execution
//
// Demonstrates end-to-end flow:
//   1. Initialize exchange clients (testnet)
//   2. Create OrderExecutor + OrderTracker
//   3. Place limit order (BTC_USDT, buy, 0.0001 @ 50000)
//   4. Track via WS private channel
//   5. Print ExecutionReport on completion
//
// Usage:
//   GATE_API_KEY=xxx GATE_API_SECRET=yyy ./test_execution
//
// NOT part of CTest — this is a manual verification tool.

#include "core/config.hpp"
#include "execution/ExecutionReport.hpp"
#include "execution/OrderExecutor.hpp"
#include "execution/OrderTracker.hpp"
#include "exchange/GateRestClient.hpp"
#include "exchange/GateWsClient.hpp"
#include "logging/Logger.hpp"
#include "market/MarketFeed.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace pulse;
using namespace pulse::execution;
using namespace pulse::exchange;
using namespace pulse::logging;
using namespace pulse::market;

/// Print ExecutionReport in human-readable format.
void print_report(const ExecutionReport &report)
{
    std::cout << "\n=== Execution Report ===" << std::endl;
    std::cout << "Order ID: " << report.order_id << std::endl;
    std::cout << "Symbol: " << report.symbol << std::endl;
    std::cout << "Side: " << (report.side == Side::Buy ? "Buy" : "Sell") << std::endl;
    std::cout << "Type: "
              << (report.type == OrderType::Market   ? "Market"
                      : report.type == OrderType::Limit  ? "Limit"
                      : report.type == OrderType::PostOnly ? "PostOnly"
                                                           : "Unknown")
              << std::endl;
    std::cout << "Requested Qty: " << report.requested_qty << std::endl;
    std::cout << "Filled Qty: " << report.filled_qty << std::endl;
    std::cout << "Avg Fill Price: " << report.avg_fill_price << std::endl;
    std::cout << "Submit Mid Price: " << report.submit_mid_price << std::endl;
    std::cout << "Slippage: " << report.slippage_bps << " bps" << std::endl;
    std::cout << "Fees: " << report.fees << std::endl;
    std::cout << "Latency: " << report.latency.count() << " ms" << std::endl;
    std::cout << "Final Status: " << (report.final_status == OrderStatus::Filled ? "Filled" : "Cancelled")
              << std::endl;
    std::cout << "========================\n" << std::endl;
}

int main()
{
    // 1. Check API credentials
    const char *env_key = std::getenv("GATE_API_KEY");
    const char *env_secret = std::getenv("GATE_API_SECRET");

    if (nullptr == env_key || nullptr == env_secret)
    {
        std::cerr << "[ERROR] GATE_API_KEY and GATE_API_SECRET environment variables required" << std::endl;
        std::cerr << "Usage: GATE_API_KEY=xxx GATE_API_SECRET=yyy ./test_execution" << std::endl;
        return 1;
    }

    // 2. Initialize logging
    LogConfig log_cfg;
    log_cfg.level = "debug";
    log_cfg.toConsole = true;
    log_cfg.toFile = false;
    Logger::init(log_cfg);

    // 3. Build exchange config (testnet)
    ExchangeConfig exchange_cfg;
    exchange_cfg.apiKey = env_key;
    exchange_cfg.apiSecret = env_secret;
    exchange_cfg.restBaseUrl = "https://fx-api-testnet.gateio.ws"; // Testnet
    exchange_cfg.wsUrl = "wss://fx-ws-testnet.gateio.ws/v4/";       // Testnet WS

    std::cout << "[INFO] Using Gate.io testnet" << std::endl;

    // 4. Create REST and WS clients
    GateRestClient rest_client(exchange_cfg);
    GateWsClient ws_client(exchange_cfg);

    // 5. Start WS client
    std::cout << "[INFO] Starting WebSocket client..." << std::endl;
    ws_client.start();

    // 6. Create MarketFeed to get current mid-price
    MarketFeed feed(ws_client, rest_client);
    std::cout << "[INFO] Starting MarketFeed for BTC_USDT..." << std::endl;
    feed.start({ "BTC_USDT" });

    // Wait for ticker data
    std::cout << "[INFO] Waiting for market data..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));

    const auto ticker = feed.tickerCache().get("BTC_USDT");
    if (!ticker.has_value())
    {
        std::cerr << "[ERROR] Failed to get BTC_USDT ticker" << std::endl;
        feed.stop();
        ws_client.stop();
        Logger::shutdown();
        return 1;
    }

    const Price mid_price = (ticker->bid + ticker->ask) / 2.0;
    std::cout << "[INFO] Current BTC_USDT mid-price: " << mid_price << std::endl;

    // 7. Create OrderExecutor and OrderTracker
    OrderExecutor executor(rest_client);
    OrderTracker tracker(ws_client, rest_client);

    // 8. Set completion callback
    std::atomic<bool> order_completed{ false };
    ExecutionReport final_report;

    tracker.setCompletionCallback([&](const ExecutionReport &report)
    {
        final_report = report;
        order_completed.store(true);
    });

    // 9. Place limit order (buy 0.0001 BTC at 95% of mid-price to ensure it rests in book)
    OrderRequest req;
    req.symbol = "BTC_USDT";
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.quantity = 0.0001;
    req.price = mid_price * 0.95; // 5% below mid to ensure it rests
    req.client_order_id = "test_exec_001";

    std::cout << "[INFO] Placing limit order: buy 0.0001 BTC @ " << req.price << std::endl;

    auto order_result = executor.placeOrder(req);
    if (!ok(order_result))
    {
        std::cerr << "[ERROR] Failed to place order: " << error(order_result).message << std::endl;
        feed.stop();
        ws_client.stop();
        Logger::shutdown();
        return 1;
    }

    const auto &order_resp = value(order_result);
    std::cout << "[OK] Order placed: id=" << order_resp.order_id << std::endl;

    // 10. Start tracking the order
    tracker.trackOrder(order_resp.order_id, req.symbol, req.side, req.type, req.quantity, mid_price);

    // 11. Wait for order completion (up to 30 seconds)
    std::cout << "[INFO] Waiting for order completion (up to 30s)..." << std::endl;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (!order_completed.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Poll status every 5 seconds as fallback
        static int poll_counter = 0;
        if (++poll_counter >= 10) // 10 * 500ms = 5s
        {
            poll_counter = 0;
            auto poll_result = tracker.pollOrderStatus(order_resp.order_id);
            if (ok(poll_result))
            {
                const auto status = value(poll_result);
                std::cout << "[INFO] Order status: "
                          << (status == OrderStatus::Open          ? "Open"
                                  : status == OrderStatus::Filled  ? "Filled"
                                  : status == OrderStatus::Cancelled ? "Cancelled"
                                                                     : "Pending")
                          << std::endl;
            }
        }
    }

    // 12. Report results
    if (order_completed.load())
    {
        std::cout << "[OK] Order completed" << std::endl;
        print_report(final_report);
    }
    else
    {
        std::cout << "[WARN] Order did not complete within 30s" << std::endl;

        // Cancel the order
        std::cout << "[INFO] Cancelling order..." << std::endl;
        const bool cancel_ok = executor.cancelOrder(order_resp.order_id);
        if (cancel_ok)
        {
            std::cout << "[OK] Order cancelled" << std::endl;
        }
        else
        {
            std::cerr << "[ERROR] Failed to cancel order" << std::endl;
        }

        // Wait a bit for cancellation to process
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Check if report was generated
        const auto report_opt = tracker.getReport(order_resp.order_id);
        if (report_opt.has_value())
        {
            print_report(*report_opt);
        }
    }

    // 13. Cleanup
    std::cout << "[INFO] Stopping MarketFeed..." << std::endl;
    feed.stop();

    std::cout << "[INFO] Stopping WebSocket client..." << std::endl;
    ws_client.stop();

    Logger::shutdown();
    std::cout << "[INFO] Done" << std::endl;

    return 0;
}
