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
// What it tests:
//   1. GET /api/v4/spot/currencies       — public, no auth
//   2. GET /api/v4/spot/currency_pairs   — public, no auth
//   3. GET /api/v4/spot/tickers           — public, with query param
//   4. GET /api/v4/spot/accounts          — private, requires API key + secret

#include "core/config.hpp"
#include "core/error.hpp"
#include "exchange/gate_rest_client.hpp"
#include "logging/logger.hpp"

#include <cstdlib>
#include <iostream>

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

        // Print first 500 chars of the response for verification
        const std::string preview = json.dump(2);
        std::cout << "       " << preview.substr(0, std::min(preview.size(), std::size_t{500}));
        if (preview.size() > 500)
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

int main()
{
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

    // 4. Test public endpoints
    std::cout << "=== Public Endpoints ===\n\n";

    print_result("GET /spot/currencies (first 3)", client.get_currencies());
    print_result("GET /spot/currency_pairs (first 3)", client.get_currency_pairs());
    print_result("GET /spot/tickers?currency_pair=BTC_USDT", client.get_ticker("BTC_USDT"));

    // 5. Test authenticated endpoints
    std::cout << "=== Authenticated Endpoints ===\n\n";

    if (client.has_credentials())
    {
        print_result("GET /spot/accounts", client.get_spot_accounts());
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
