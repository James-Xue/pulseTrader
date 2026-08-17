// test_gate_rest.cpp — Smoke test for Gate.io REST client (Layer 1 Exchange)
//
// Manual verification tool — NOT part of CTest (requires network access).
//
// Usage:
//   # Public endpoints only (no API key needed):
//   ./build/tools/test_gate_rest
//
//   # With API credentials (for authenticated endpoints):
//   GATE_API_KEY=xxx GATE_API_SECRET=yyy ./build/tools/test_gate_rest
//
//   # TradFi (CFD) live-API probe — verifies the order schema, field names
//   # and close-endpoint shapes before the engine integration is finalised.
//   # Places a SAFE trigger (limit) buy at 3000 that cannot fill, then
//   # queries and cancels it. Requires an API key with the CFD permission:
//   GATE_API_KEY=xxx GATE_API_SECRET=yyy ./build/tools/test_gate_rest --tradfi
//
// What it tests:
//   1. GET /api/v4/spot/currencies       — public, no auth
//   2. GET /api/v4/spot/currency_pairs   — public, no auth
//   3. GET /api/v4/spot/tickers           — public, with query param
//   4. GET /api/v4/spot/accounts          — private, requires API key + secret
//   5. --tradfi: symbols/tickers/klines/detail/assets/orders + safe order probe

#include "core/config.hpp"
#include "core/PulseError.hpp"
#include "exchange/EndpointRouter.hpp"
#include "exchange/GateRestClient.hpp"
#include "logging/Logger.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace pulse;
using namespace pulse::exchange;

// Helper: print a Result<json> with a label.
void print_result(const std::string &label, const Result<nlohmann::json> &r)
{
    if (ok(r))
    {
        const auto &json = value(r);
        std::cout << "[OK]   " << label << " — ";
        if (json.is_array())
        {
            std::cout << "array of " << json.size() << " items";
        }
        else
        {
            std::cout << "object";
        }
        std::cout << "\n";

        // Print the first 2000 chars of the response for verification
        const std::string preview = json.dump(2);
        std::cout << "       " << preview.substr(0, std::min(preview.size(), std::size_t{2000}));
        if (preview.size() > 2000)
        {
            std::cout << " ... (truncated)";
        }
        std::cout << "\n\n";
    }
    else
    {
        const auto &err = error(r);
        std::cout << "[FAIL] " << label << " — code=" << static_cast<int>(err.code) << " msg=" << err.message
                  << "\n\n";
    }
}

// ---------------------------------------------------------------------------
// TradFi (CFD) live-API probe — resolves the order schema for the engine
// ---------------------------------------------------------------------------
void probe_cfd(GateRestClient &client)
{
    std::cout << "=== TradFi (CFD) Probe ===\n\n";

    // Public market data — field names for the feed parsers.
    print_result("GET /tradfi/symbols (public)", client.getCfdSymbols());
    print_result("GET /tradfi/symbols/XAUUSD/tickers (public)",
                 client.getCfdTicker("XAUUSD"));
    print_result("GET /tradfi/symbols/XAUUSD/klines?kline_type=1m&limit=3 (public)",
                 client.getCfdKlines("XAUUSD", 3));

    if (!client.hasCredentials())
    {
        std::cout << "[SKIP] authenticated CFD probes — no credentials\n\n";
        return;
    }

    // Authenticated read probes — contract spec + balance field names.
    print_result("GET /tradfi/symbols/detail?symbols=XAUUSD (auth)",
                 client.getCfdSymbolsDetail({ "XAUUSD" }));
    print_result("GET /tradfi/users/assets (auth)", client.getCfdAssets());
    print_result("GET /tradfi/positions (auth)", client.getCfdPositions());
    print_result("GET /tradfi/orders (auth)",
                 client.request("GET", EndpointRouter::ordersPath(MarketType::Cfd)));
    print_result("GET /tradfi/transactions (auth)",
                 client.request("GET", "/api/v4/tradfi/transactions"));

    // SAFE order probe: trigger (limit) BUY at 3000 — the market is ~4348 so
    // this cannot fill. Query it, then cancel it.
    std::cout << "--- SAFE order probe (unfillable trigger buy @3000) ---\n\n";
    nlohmann::json body;
    body["symbol"]     = "XAUUSD";
    body["side"]       = 2; // 2 = buy, 1 = sell (MT5 style)
    body["volume"]     = "0.01";
    body["price_type"] = "trigger";
    body["price"]      = "3000";

    auto placed = client.postCfdOrder(body);
    print_result("POST /tradfi/orders (MT5-style trigger buy @3000)", placed);

    if (ok(placed))
    {
        std::string order_id;
        const auto &pj = value(placed);
        if (pj.contains("id"))
        {
            if (pj["id"].is_number())
            {
                order_id = std::to_string(pj["id"].get<std::int64_t>());
            }
            else
            {
                order_id = pj["id"].get<std::string>();
            }
        }
        std::cout << "       order id = " << order_id << "\n\n";

        if (!order_id.empty())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            print_result("GET /tradfi/orders/" + order_id, client.getCfdOrder(order_id));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            print_result("DELETE /tradfi/orders/" + order_id,
                         client.cancelCfdOrder(order_id));
        }
    }
    else
    {
        // Fallback: retry with the spot-style variant (trading_session/tif/
        // client_order_id) — resolves the MT5-vs-spot-style open question.
        std::cout << "--- retrying with spot-style variant ---\n\n";
        nlohmann::json body2;
        body2["symbol"]          = "XAUUSD";
        body2["side"]            = "buy";
        body2["volume"]          = "0.01";
        body2["price_type"]      = "limit";
        body2["price"]           = "3000";
        body2["trading_session"] = "All";
        body2["time_in_force"]   = "day";
        body2["client_order_id"] = "probe_1";
        print_result("POST /tradfi/orders (spot-style limit buy @3000)",
                     client.request("POST",
                                    EndpointRouter::ordersPath(MarketType::Cfd),
                                    "", body2.dump()));
    }
}

// Helper: read a field as string regardless of JSON type (number/string).
std::string str_field(const nlohmann::json &o, const char *key)
{
    if (!o.contains(key))
    {
        return "";
    }
    const auto &v = o[key];
    if (v.is_string())
    {
        return v.get<std::string>();
    }
    if (v.is_number())
    {
        return std::to_string(v.get<double>());
    }
    return "";
}

// Helper: print a Result<json> WITHOUT truncation (for critical probe evidence).
void print_full(const std::string &label, const Result<nlohmann::json> &r)
{
    if (ok(r))
    {
        std::cout << "[OK]   " << label << "\n" << value(r).dump(2) << "\n\n";
    }
    else
    {
        std::cout << "[FAIL] " << label << " — code=" << static_cast<int>(error(r).code)
                  << " msg=" << error(r).message << "\n\n";
    }
}

// ---------------------------------------------------------------------------
// TradFi (CFD) market-order probe — root-causes the engine's broken market
// order path. Places a REAL 0.01-lot market buy (fills ~immediately), dumps
// every response unfiltered, then closes the position right away.
//
// Evidence captured:
//   - POST /tradfi/orders response body (2xx-with-label vs {"data":...})
//   - whether a filled market order stays in the open-orders list, and its
//     exact state/finished/fill fields (drives OrderTracker parsing)
//   - trigger order lifecycle: open state -> DELETE -> final state
// ---------------------------------------------------------------------------
void probe_cfd_market_order(GateRestClient &client)
{
    std::cout << "=== TradFi (CFD) Market-Order Probe ===\n\n";

    if (!client.hasCredentials())
    {
        std::cout << "[SKIP] no credentials\n\n";
        return;
    }

    // 1. Preconditions — balance high enough to cover 0.01 lot (~9 USD margin).
    auto assets = client.getCfdAssets();
    print_full("GET /tradfi/users/assets (pre)", assets);
    bool balance_ok = false;
    if (ok(assets))
    {
        const auto &data = value(assets).value("data", nlohmann::json::object());
        balance_ok = safeParseDouble(str_field(data, "outable")).value_or(0.0) >= 15.0;
    }
    if (!balance_ok)
    {
        std::cout << "[ABORT] CFD balance < 15 USD — deposit first.\n\n";
        return;
    }

    // 2. Snapshot the current open orders + positions.
    print_full("GET /tradfi/orders (pre)",
               client.request("GET", EndpointRouter::ordersPath(MarketType::Cfd)));
    print_full("GET /tradfi/positions (pre)", client.getCfdPositions());

    // 3. Market order POST — EXACTLY the engine's body (no price field).
    std::cout << "--- Market order POST (engine-identical body) ---\n\n";
    nlohmann::json body;
    body["symbol"]     = "XAUUSD";
    body["side"]       = 2; // 2 = buy, 1 = sell (MT5 style)
    body["volume"]     = "0.01";
    body["price_type"] = "market";
    print_full("POST /tradfi/orders {symbol,side:2,volume:0.01,price_type:market}",
               client.postCfdOrder(body));

    // 4. Post-POST evidence — is the order in the open list? Did a position open?
    std::this_thread::sleep_for(std::chrono::seconds(2));
    print_full("GET /tradfi/orders (after market POST)",
               client.request("GET", EndpointRouter::ordersPath(MarketType::Cfd)));
    auto positions = client.getCfdPositions();
    print_full("GET /tradfi/positions (after market POST)", positions);

    // 5. Close any XAUUSD position the market order opened (full close).
    if (ok(positions))
    {
        const auto &data = value(positions).value("data", nlohmann::json::object());
        const auto &list = data.value("list", nlohmann::json::array());
        if (list.is_array())
        {
            for (const auto &p : list)
            {
                // position_id is a NUMBER in the API payload — value() would
                // silently return "" and skip every close (no-op, 2026-08-17).
                const std::string pid = str_field(p, "position_id");
                if (!pid.empty())
                {
                    print_full("POST /tradfi/positions/" + pid + "/close (close_type=2)",
                               client.postCfdPositionClose(pid, 2, 0.0));
                }
            }
        }
    }

    // 6. Trigger lifecycle — open state, then DELETE, then final state.
    std::cout << "--- Trigger order lifecycle ---\n\n";
    nlohmann::json trig;
    trig["symbol"]     = "XAUUSD";
    trig["side"]       = 2;
    trig["volume"]     = "0.01";
    trig["price_type"] = "trigger";
    trig["price"]      = "3000"; // Unfillable — far below the market.
    print_full("POST /tradfi/orders (trigger buy @3000)", client.postCfdOrder(trig));

    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto trig_list = client.request("GET", EndpointRouter::ordersPath(MarketType::Cfd));
    print_full("GET /tradfi/orders (trigger open state)", trig_list);

    // Find the trigger we just placed (newest XAUUSD buy 0.01 trigger @3000).
    std::string trig_id;
    if (ok(trig_list))
    {
        const auto &data = value(trig_list).value("data", nlohmann::json::object());
        const auto &list = data.value("list", nlohmann::json::array());
        if (list.is_array())
        {
            for (const auto &o : list)
            {
                if (o.value("symbol", "") != "XAUUSD") continue;
                if (o.value("side", 0) != 2) continue;
                if (std::abs(safeParseDouble(str_field(o, "price")).value_or(0.0) - 3000.0) > 0.01) continue;
                if (o.contains("order_id"))
                {
                    trig_id = o["order_id"].is_number()
                        ? std::to_string(o["order_id"].get<std::int64_t>())
                        : o["order_id"].get<std::string>();
                }
                break;
            }
        }
    }
    if (!trig_id.empty())
    {
        std::cout << "       trigger order id = " << trig_id << "\n\n";
        print_full("DELETE /tradfi/orders/" + trig_id, client.cancelCfdOrder(trig_id));
        std::this_thread::sleep_for(std::chrono::seconds(1));
        print_full("GET /tradfi/orders (after trigger DELETE)",
                   client.request("GET", EndpointRouter::ordersPath(MarketType::Cfd)));
    }

    // 7. Final balance.
    print_full("GET /tradfi/users/assets (post)", client.getCfdAssets());
}

// ---------------------------------------------------------------------------
// TradFi (CFD) cleanup — cancel all active orders and withdraw the full CFD
// balance back to the main account. Used when the CFD direction is parked.
// ---------------------------------------------------------------------------
void probe_cfd_cleanup(GateRestClient &client)
{
    std::cout << "=== TradFi (CFD) Cleanup ===\n\n";

    if (!client.hasCredentials())
    {
        std::cout << "[SKIP] no credentials\n\n";
        return;
    }

    // 1. Cancel every active order.
    auto orders = client.request("GET", EndpointRouter::ordersPath(MarketType::Cfd));
    if (ok(orders))
    {
        const auto &data = value(orders).value("data", nlohmann::json::object());
        const auto &list = data.value("list", nlohmann::json::array());
        if (list.is_array())
        {
            for (const auto &o : list)
            {
                std::string id;
                if (o.contains("order_id"))
                {
                    if (o["order_id"].is_number())
                    {
                        id = std::to_string(o["order_id"].get<std::int64_t>());
                    }
                    else
                    {
                        id = o["order_id"].get<std::string>();
                    }
                }
                if (!id.empty())
                {
                    print_result("DELETE /tradfi/orders/" + id + " (cleanup)",
                                 client.cancelCfdOrder(id));
                }
            }
        }
    }

    // 2. Withdraw the full withdrawable balance (outable) to the main account.
    auto assets = client.getCfdAssets();
    if (ok(assets))
    {
        const auto &data = value(assets).value("data", nlohmann::json::object());
        const std::string outable = data.value("outable", "0");
        const double amount = safeParseDouble(outable).value_or(0.0);
        if (amount > 0.0)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.2f", amount);
            print_result("POST /tradfi/transactions (withdraw "
                             + std::string(buf) + " USDT -> main)",
                         client.postCfdTransfer("USDT", buf, "withdraw"));
        }
        else
        {
            std::cout << "[SKIP] withdraw — CFD balance is zero\n\n";
        }
    }

    // 3. Verify the final state.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    print_result("GET /tradfi/orders (verify empty)",
                 client.request("GET", EndpointRouter::ordersPath(MarketType::Cfd)));
    print_result("GET /tradfi/users/assets (verify zero)", client.getCfdAssets());
}

int main(int argc, char *argv[])
{
    const bool run_cfd = (argc > 1 && std::string("--tradfi") == argv[1]);
    const bool run_cleanup = (argc > 1 && std::string("--tradfi-cleanup") == argv[1]);
    const bool run_market = (argc > 1 && std::string("--tradfi-market") == argv[1]);

    // 1. Initialise logging (console only, info level)
    LogConfig log_cfg;
    log_cfg.toConsole = true;
    log_cfg.toFile = false;
    log_cfg.level = "info";
    logging::Logger::init(log_cfg);

    // 2. Build exchange config from environment variables
    ExchangeConfig exchange_cfg;
    exchange_cfg.restBaseUrl = "https://api.gateio.ws";

    const char *env_key = std::getenv("GATE_API_KEY");
    const char *env_secret = std::getenv("GATE_API_SECRET");
    if (env_key && env_secret)
    {
        exchange_cfg.apiKey = env_key;
        exchange_cfg.apiSecret = env_secret;
        std::cout << "API credentials loaded from environment.\n\n";
    }
    else
    {
        std::cout << "No GATE_API_KEY/GATE_API_SECRET in environment.\n";
        std::cout << "Only public endpoints will be tested.\n\n";
    }

    // 3. Create client
    GateRestClient client(exchange_cfg);

    if (run_cfd)
    {
        probe_cfd(client);
        logging::Logger::shutdown();
        std::cout << "Done.\n";
        return 0;
    }

    if (run_cleanup)
    {
        probe_cfd_cleanup(client);
        logging::Logger::shutdown();
        std::cout << "Done.\n";
        return 0;
    }

    if (run_market)
    {
        probe_cfd_market_order(client);
        logging::Logger::shutdown();
        std::cout << "Done.\n";
        return 0;
    }

    // 4. Test public endpoints
    std::cout << "=== Public Endpoints ===\n\n";

    print_result("GET /spot/currencies (first 3)", client.getCurrencies());
    print_result("GET /spot/currency_pairs (first 3)", client.getCurrencyPairs());
    print_result("GET /spot/tickers?currency_pair=BTC_USDT", client.getTicker("BTC_USDT"));

    // 5. Test authenticated endpoints
    std::cout << "=== Authenticated Endpoints ===\n\n";

    if (client.hasCredentials())
    {
        print_result("GET /spot/accounts", client.getSpotAccounts());
    }
    else
    {
        std::cout << "[SKIP] GET /spot/accounts — no credentials\n\n";
    }

    // 6. Clean up
    logging::Logger::shutdown();
    std::cout << "Done.\n";
    return 0;
}
