// test_balance_diag.cpp — Diagnostic tool for futures account balance verification
//
// Purpose: Verify that the program's parsed account balance matches the raw
//          Gate.io API response, and provide data for comparison with the
//          Gate.io web backend.
//
// Usage:
//   ./build/tools/test_balance_diag                    # auto-loads trading.toml
//   ./build/tools/test_balance_diag --config trading.toml
//
// Output:
//   1. Config info (network, URL, masked API key)
//   2. Raw JSON from Gate.io futures account endpoint
//   3. Parsed AccountBalance values
//   4. Field-by-field match verification

#include "core/config.hpp"
#include "core/config_loader.hpp"
#include "core/error.hpp"
#include "core/types.hpp"
#include "exchange/gate_rest_client.hpp"
#include "logging/logger.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>

using namespace pulse;
using namespace pulse::exchange;

int main(int argc, char *argv[])
{
    // --- Minimal logging ---
    LogConfig log_cfg;
    log_cfg.toConsole = false;
    log_cfg.toFile = false;
    logging::Logger::init(log_cfg);

    // --- Load config ---
    std::string config_path = "trading.toml";
    if (argc > 2 && std::string(argv[1]) == "--config")
    {
        config_path = argv[2];
    }

    PulseConfig cfg;
    bool config_loaded = false;
    auto load_result = load_config_file(config_path);
    if (ok(load_result))
    {
        cfg = value(load_result);
        config_loaded = true;
    }
    else
    {
        // Fallback: build from env vars
        std::string network = "mainnet";
        if (const char *n = std::getenv("PULSE_NETWORK"); n)
            network = n;

        if ("testnet" == network)
        {
            cfg.exchange.testnet = true;
            cfg.exchange.restBaseUrl = "https://api-testnet.gateapi.io";
            if (const char *k = std::getenv("GATE_TESTNET_API_KEY"); k)
                cfg.exchange.apiKey = k;
            if (const char *s = std::getenv("GATE_TESTNET_API_SECRET"); s)
                cfg.exchange.apiSecret = s;
        }
        else
        {
            cfg.exchange.restBaseUrl = "https://api.gateio.ws";
            if (const char *k = std::getenv("GATE_MAINNET_API_KEY"); k)
                cfg.exchange.apiKey = k;
            else if (const char *k2 = std::getenv("GATE_API_KEY"); k2)
                cfg.exchange.apiKey = k2;
            if (const char *s = std::getenv("GATE_MAINNET_API_SECRET"); s)
                cfg.exchange.apiSecret = s;
            else if (const char *s2 = std::getenv("GATE_API_SECRET"); s2)
                cfg.exchange.apiSecret = s2;
        }
    }

    // Proxy: explicit config → env var fallback
    if (cfg.exchange.proxyUrl.empty())
    {
        if (const char *p = std::getenv("HTTPS_PROXY"); p)
            cfg.exchange.proxyUrl = p;
        else if (const char *p2 = std::getenv("HTTP_PROXY"); p2)
            cfg.exchange.proxyUrl = p2;
    }

    // --- Display config ---
    std::string masked_key = cfg.exchange.apiKey.size() > 8
                                 ? cfg.exchange.apiKey.substr(0, 4) + "****" + cfg.exchange.apiKey.substr(cfg.exchange.apiKey.size() - 4)
                                 : "(empty)";

    std::string full_url = cfg.exchange.restBaseUrl + "/api/v4/futures/usdt/accounts";

    std::cout << "=== pulseTrader Balance Diagnostic ===\n\n";
    std::cout << "Config source : " << (config_loaded ? config_path : "env vars (no TOML)") << "\n";
    std::cout << "Network       : " << (cfg.exchange.testnet ? "TESTNET" : "MAINNET") << "\n";
    std::cout << "REST base URL : " << cfg.exchange.restBaseUrl << "\n";
    std::cout << "Full endpoint : " << full_url << "\n";
    std::cout << "API Key       : " << masked_key << "\n";
    std::cout << "Proxy         : " << (cfg.exchange.proxyUrl.empty() ? "(none)" : cfg.exchange.proxyUrl) << "\n";
    std::cout << "\n";

    // --- Create REST client ---
    GateRestClient client(cfg.exchange, MarketType::Futures);

    // --- Test 1: Raw JSON from Gate.io ---
    std::cout << "--- [Test 1] Raw API Response ---\n\n";
    auto raw_result = client.get_futures_accounts();

    nlohmann::json raw_json;

    if (ok(raw_result))
    {
        raw_json = value(raw_result);
        std::cout << raw_json.dump(2) << "\n\n";
    }
    else
    {
        std::cout << "[FAIL] " << error(raw_result).message << "\n\n";
        std::cout << "Check: API key/secret, network mode, proxy, firewall.\n";
        logging::Logger::shutdown();
        return 1;
    }

    // --- Test 2: Parsed AccountBalance ---
    std::cout << "--- [Test 2] Parsed AccountBalance ---\n\n";
    auto bal_result = client.get_futures_account_balance();

    if (!ok(bal_result))
    {
        std::cout << "[FAIL] " << error(bal_result).message << "\n";
        logging::Logger::shutdown();
        return 1;
    }

    const auto &bal = value(bal_result);
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "Total           : " << bal.total << " " << bal.currency << "\n";
    std::cout << "Available       : " << bal.available << " " << bal.currency << "\n";
    std::cout << "Unrealised PnL  : " << bal.unrealised_pnl << " " << bal.currency << "\n";
    std::cout << "Position Margin : " << bal.position_margin << " " << bal.currency << "\n";
    std::cout << "Order Margin    : " << bal.order_margin << " " << bal.currency << "\n";
    std::cout << "\n";

    // --- Test 3: Verify parsing correctness ---
    std::cout << "--- [Test 3] Parsing Verification ---\n\n";

    auto check_field = [](const std::string &name, double parsed, const nlohmann::json &j, const std::string &key)
    {
        std::string raw_str = j.value(key, "MISSING");
        double raw_val = 0.0;
        if ("MISSING" != raw_str)
        {
            auto r = safe_parse_double(raw_str);
            raw_val = r.value_or(-999.0);
        }
        bool match = ("MISSING" == raw_str) ? false : (std::abs(parsed - raw_val) < 0.0001);
        std::cout << "  " << std::left << std::setw(20) << name
                  << "  raw=\"" << raw_str << "\""
                  << "  parsed=" << std::fixed << std::setprecision(4) << parsed
                  << "  " << (match ? "OK" : "MISMATCH") << "\n";
    };

    check_field("total", bal.total, raw_json, "total");
    check_field("available", bal.available, raw_json, "available");
    check_field("unrealised_pnl", bal.unrealised_pnl, raw_json, "unrealised_pnl");
    check_field("position_margin", bal.position_margin, raw_json, "position_margin");
    check_field("order_margin", bal.order_margin, raw_json, "order_margin");

    std::cout << "\n";

    // --- Gate.io API field notes ---
    std::cout << "--- Notes ---\n";
    std::cout << "Gate.io 'total' = deposits - withdrawals + realized PnL - fees";
    if (raw_json.contains("unrealised_pnl"))
    {
        double total = safe_parse_double(raw_json.value("total", "0")).value_or(0.0);
        double upnl = safe_parse_double(raw_json.value("unrealised_pnl", "0")).value_or(0.0);
        std::cout << "\nGate.io 'total' does NOT include unrealised_pnl.";
        std::cout << "\nActual equity = total + unrealised_pnl = "
                  << std::fixed << std::setprecision(4) << (total + upnl) << " USDT";
    }
    std::cout << "\n\nCompare above values with Gate.io web backend.\n";

    logging::Logger::shutdown();
    return 0;
}
