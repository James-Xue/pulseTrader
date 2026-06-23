// test_balance_diag.cpp — Diagnostic tool for account balance verification
//
// Purpose: Query BOTH spot and futures accounts to verify the API key
//          points to the correct account, and compare with Gate.io web backend.
//
// Usage:
//   ./build/tools/test_balance_diag                    # auto-loads trading.toml
//   ./build/tools/test_balance_diag --config trading.toml

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
    auto load_result = loadConfigFile(config_path);
    if (ok(load_result))
    {
        cfg = value(load_result);
        config_loaded = true;
    }
    else
    {
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

    // Proxy
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

    std::cout << "=== pulseTrader Balance Diagnostic ===\n\n";
    std::cout << "Config source : " << (config_loaded ? config_path : "env vars (no TOML)") << "\n";
    std::cout << "Network       : " << (cfg.exchange.testnet ? "TESTNET" : "MAINNET") << "\n";
    std::cout << "REST base URL : " << cfg.exchange.restBaseUrl << "\n";
    std::cout << "API Key       : " << masked_key << "\n";
    std::cout << "Proxy         : " << (cfg.exchange.proxyUrl.empty() ? "(none)" : cfg.exchange.proxyUrl) << "\n";
    std::cout << "\n";

    // =====================================================================
    // Futures Account
    // =====================================================================
    std::cout << "========== FUTURES ACCOUNT ==========\n\n";

    GateRestClient futures_client(cfg.exchange, MarketType::Futures);

    std::cout << "Endpoint: " << cfg.exchange.restBaseUrl << "/api/v4/futures/usdt/accounts\n\n";

    auto futures_raw = futures_client.getFuturesAccounts();
    if (ok(futures_raw))
    {
        const auto &j = value(futures_raw);
        std::cout << "--- Raw JSON ---\n";
        std::cout << j.dump(2) << "\n\n";

        if (j.contains("user"))
        {
            std::cout << ">>> User ID: " << j["user"] << "\n";
            std::cout << "    (verify this matches your Gate.io testnet account)\n\n";
        }

        // Parsed values
        auto bal_result = futures_client.getFuturesAccountBalance();
        if (ok(bal_result))
        {
            const auto &bal = value(bal_result);
            std::cout << std::fixed << std::setprecision(4);
            std::cout << "--- Parsed ---\n";
            std::cout << "Total           : " << bal.total << " " << bal.currency << "\n";
            std::cout << "Available       : " << bal.available << " " << bal.currency << "\n";
            std::cout << "Unrealised PnL  : " << bal.unrealised_pnl << " " << bal.currency << "\n";
            std::cout << "Position Margin : " << bal.position_margin << " " << bal.currency << "\n";
            std::cout << "Order Margin    : " << bal.order_margin << " " << bal.currency << "\n";

            // Equity calculation
            double total = safeParseDouble(j.value("total", "0")).value_or(0.0);
            double upnl = safeParseDouble(j.value("unrealised_pnl", "0")).value_or(0.0);
            std::cout << "\nEquity (total + unrealised_pnl) = " << (total + upnl) << " " << bal.currency << "\n";
        }
    }
    else
    {
        std::cout << "[FAIL] " << error(futures_raw).message << "\n";
    }

    // =====================================================================
    // Spot Account
    // =====================================================================
    std::cout << "\n========== SPOT ACCOUNT ==========\n\n";

    GateRestClient spot_client(cfg.exchange, MarketType::Spot);

    std::cout << "Endpoint: " << cfg.exchange.restBaseUrl << "/api/v4/spot/accounts\n\n";

    auto spot_raw = spot_client.getSpotAccounts();
    if (ok(spot_raw))
    {
        const auto &j = value(spot_raw);

        // Spot accounts return an array — show USDT-related entries
        std::cout << "--- Raw JSON (array of " << j.size() << " items) ---\n";

        // Find and highlight USDT
        bool found_usdt = false;
        for (const auto &item : j)
        {
            std::string currency = item.value("currency", "");
            if ("USDT" == currency)
            {
                found_usdt = true;
                std::cout << "\n>>> USDT Spot Account:\n";
                std::cout << item.dump(2) << "\n";

                std::string avail_str = item.value("available", "0");
                std::string locked_str = item.value("locked", "0");
                double avail = safeParseDouble(avail_str).value_or(0.0);
                double locked = safeParseDouble(locked_str).value_or(0.0);

                std::cout << std::fixed << std::setprecision(4);
                std::cout << "\n  Available : " << avail << " USDT\n";
                std::cout << "  Locked    : " << locked << " USDT\n";
                std::cout << "  Total     : " << (avail + locked) << " USDT\n";
            }
        }

        if (!found_usdt)
        {
            std::cout << "(No USDT spot account found)\n";
            std::cout << "All currencies: ";
            for (const auto &item : j)
            {
                std::cout << item.value("currency", "?") << " ";
            }
            std::cout << "\n";
        }
    }
    else
    {
        std::cout << "[FAIL] " << error(spot_raw).message << "\n";
    }

    // =====================================================================
    // Summary
    // =====================================================================
    std::cout << "\n========== SUMMARY ==========\n\n";
    std::cout << "Compare the User ID and balance values above with what you see\n";
    std::cout << "in the Gate.io testnet web backend (fx-testnet.gate.com).\n\n";
    std::cout << "If User ID does not match → API key is for a different account.\n";
    std::cout << "If User ID matches but balance differs → possible API caching issue.\n";

    logging::Logger::shutdown();
    return 0;
}
