// test_control_smoke.cpp — Manual smoke test for the control plane
//
// Connects to a running engine's control socket and exercises the
// JSON-RPC method set. NOT part of CTest (requires a running engine).
//
// Usage:
//   ./build/tools/test_control_smoke [--host 127.0.0.1] [--port 8081]

#include "control/ControlClient.hpp"

#include "core/config.hpp"
#include "core/config_loader.hpp"
#include "logging/Logger.hpp"

#include <cstdint>
#include <iostream>
#include <string>

using namespace pulse;

int main(int argc, char *argv[])
{
    std::string host = "127.0.0.1";
    std::uint16_t port = 8081;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg(argv[i]);
        if ("--host" == arg && i + 1 < argc)
        {
            host = argv[++i];
        }
        else if ("--port" == arg && i + 1 < argc)
        {
            port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        }
    }

    // Lightweight logging (file only; nothing on stdout).
    pulse::LogConfig log_cfg;
    log_cfg.toConsole = false;
    log_cfg.logDir = "logs";
    pulse::logging::Logger::init(log_cfg);

    pulse::control::ControlClient client;
    if (!client.connect(host, port))
    {
        std::cerr << "[FAIL] Cannot connect to " << host << ":" << port
                  << " — is the engine running?\n";
        return 1;
    }
    std::cout << "[OK] Connected to " << host << ":" << port << "\n";

    int failures = 0;
    auto check = [&failures](const std::string &label, bool ok_)
    {
        std::cout << (ok_ ? "[OK]   " : "[FAIL] ") << label << "\n";
        if (!ok_)
        {
            ++failures;
        }
    };

    // 1. status
    {
        auto r = client.call("get_status");
        check("get_status", ok(r));
        if (ok(r))
        {
            std::cout << "       " << value(r).dump() << "\n";
        }
    }

    // 2. strategies
    {
        auto r = client.call("list_strategies");
        check("list_strategies", ok(r));
    }

    // 3. risk
    {
        auto r = client.call("get_risk");
        check("get_risk", ok(r));
    }

    // 4. positions (may be empty)
    {
        auto r = client.call("get_positions");
        check("get_positions", ok(r));
    }

    // 5. orders (may be empty)
    {
        auto r = client.call("get_orders");
        check("get_orders", ok(r));
    }

    // 6. halt/resume round-trip
    {
        auto h = client.call("halt_trading");
        check("halt_trading", ok(h));
        auto r = client.call("resume_trading");
        check("resume_trading", ok(r));
    }

    // 7. unknown method → JSON-RPC error
    {
        auto r = client.call("no_such_method");
        check("unknown method rejected", !ok(r));
    }

    std::cout << (0 == failures ? "[OK] Smoke test passed\n"
                                : "[FAIL] Smoke test failed\n");
    return 0 == failures ? 0 : 1;
}
