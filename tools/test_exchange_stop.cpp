// test_exchange_stop.cpp — Testnet probe for exchange-side protective stops (audit B1)
//
// Manual verification tool — NOT part of CTest (network + testnet account).
//
// What it verifies, in order:
//   1. Testnet REST credentials work (futures account balance)
//   2. A 1-contract market BUY can be opened
//   3. The protective price_orders body the engine builds is ACCEPTED:
//      whole-position close, reduce-only, auto_size=close, rule=2 (long SL),
//      trigger price rounded to the contract's order_price_round
//   4. The trigger appears in the open price-orders list
//   5. Cleanup: cancel the trigger, then close ONLY the contracts this probe
//      opened (the account may already hold a position)
//
// Usage:
//   set -a; . ./.env; set +a
//   PULSE_NETWORK=testnet ./build_main/tools/test_exchange_stop --dry-run
//   PULSE_NETWORK=testnet ./build_main/tools/test_exchange_stop --place
//
// Credentials (environment): GATE_TESTNET_API_KEY / GATE_TESTNET_API_SECRET.
//
// Safety:
//   - Refuses to run unless PULSE_NETWORK=testnet is set explicitly.
//   - --dry-run performs only read-only calls; --place is required for the
//     order/trigger sequence (virtual funds, but still real API calls).

#include "core/PulseError.hpp"
#include "core/config.hpp"
#include "exchange/GateRestClient.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace pulse;
using namespace pulse::exchange;

namespace
{

constexpr const char *kContract = "BTC_USDT";

int fail(const std::string &step, const PulseError &err)
{
    std::cout << "[FAIL] " << step << " — " << err.message << "\n";
    return 1;
}

std::string envOr(const char *name, const std::string &fallback = "")
{
    const char *v = std::getenv(name);
    return (nullptr == v || '\0' == *v) ? fallback : std::string(v);
}

/// Current position size for kContract (0 when flat).
int positionSize(GateRestClient &client)
{
    auto positions = client.getFuturesPositions();
    if (!ok(positions))
    {
        return 0;
    }
    for (const auto &p : value(positions))
    {
        if (kContract == p.value("contract", ""))
        {
            return p.value("size", 0);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    bool place = false;
    bool cancel_triggers = false;
    int close_n = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if ("--place" == arg)
        {
            place = true;
        }
        else if ("--dry-run" == arg)
        {
            place = false;
        }
        else if ("--cancel-triggers" == arg)
        {
            cancel_triggers = true;
        }
        else if ("--close" == arg && i + 1 < argc)
        {
            close_n = std::stoi(argv[++i]);
        }
    }

    // Safety gate: only testnet, and only when explicitly requested.
    if ("testnet" != envOr("PULSE_NETWORK"))
    {
        std::cout << "[REFUSE] PULSE_NETWORK must be explicitly set to "
                     "\"testnet\" for this probe.\n";
        return 2;
    }

    ExchangeConfig cfg;
    cfg.testnet = true;
    cfg.restBaseUrl = url::kTestnetRest;
    cfg.apiKey = envOr("GATE_TESTNET_API_KEY", envOr("GATE_API_KEY"));
    cfg.apiSecret = envOr("GATE_TESTNET_API_SECRET", envOr("GATE_API_SECRET"));
    cfg.restTimeoutMs = 10000;
    cfg.maxRetries = 3;

    if (cfg.apiKey.empty() || cfg.apiSecret.empty())
    {
        std::cout << "[REFUSE] Missing testnet credentials "
                     "(GATE_TESTNET_API_KEY / GATE_TESTNET_API_SECRET).\n";
        return 2;
    }

    GateRestClient client(cfg, MarketType::Futures);

    std::cout << "=== exchange-stop probe (testnet, " << (place ? "--place" : "--dry-run")
              << ") ===\n";

    // 1. Credentials / account.
    auto balance = client.getFuturesAccountBalance();
    if (!ok(balance))
    {
        return fail("futures balance", error(balance));
    }
    std::cout << "[OK]   balance: total=" << value(balance).total
              << " " << value(balance).currency
              << " available=" << value(balance).available << "\n";

    // Manual cleanup mode for a leftover probe position.
    if (close_n > 0)
    {
        nlohmann::json close_body{
            { "contract", kContract },
            { "size", -close_n },
            { "price", "0" },
            { "tif", "ioc" },
            { "reduce_only", true },
        };
        auto closed = client.postFuturesOrder(close_body);
        if (!ok(closed))
        {
            return fail("cleanup close", error(closed));
        }
        std::cout << "[OK]   closed " << close_n << " contract(s)\n";
        return 0;
    }

    // Manual cleanup mode: cancel every open trigger on the contract.
    if (cancel_triggers)
    {
        auto triggers = client.getFuturesPriceOrders(kContract);
        if (!ok(triggers))
        {
            return fail("list price orders", error(triggers));
        }
        std::size_t seen = 0;
        for (const auto &t : value(triggers))
        {
            // Gate ids exceed int32 — read them as int64 or they truncate.
            const auto id_value = t.contains("id") ? t["id"].get<std::int64_t>() : 0;
            const auto id = std::to_string(id_value);
            std::cout << "[INFO] trigger " << id << " "
                      << t.dump().substr(0, 220) << "\n";
            auto cancelled = client.cancelFuturesPriceOrder(id);
            std::cout << (ok(cancelled) ? "[OK]   cancelled " : "[WARN] cancel failed ")
                      << id << "\n";
            ++seen;
        }
        std::cout << "[INFO] triggers seen: " << seen << "\n";
        return 0;
    }

    // 2. Contract detail (quanto multiplier, price tick).
    auto contracts = client.getFuturesContracts();
    if (!ok(contracts))
    {
        return fail("contract list", error(contracts));
    }
    bool found = false;
    double quanto = 1.0;
    double price_tick = 0.0;
    for (const auto &c : value(contracts))
    {
        if (kContract == c.value("name", ""))
        {
            found = true;
            quanto = std::stod(c.value("quanto_multiplier", "1"));
            price_tick = std::stod(c.value("order_price_round", "0"));
            break;
        }
    }
    if (!found)
    {
        std::cout << "[FAIL] contract " << kContract << " not listed on testnet\n";
        return 1;
    }
    std::cout << "[OK]   contract " << kContract << " quanto=" << quanto
              << " price_tick=" << price_tick << "\n";

    std::cout << "[INFO] pre-existing position size=" << positionSize(client) << "\n";

    if (!place)
    {
        std::cout << "[SKIP] order/trigger sequence — rerun with --place\n";
        return 0;
    }

    if (price_tick <= 0.0)
    {
        std::cout << "[FAIL] contract has no order_price_round — cannot build a "
                     "valid trigger price\n";
        return 1;
    }

    // Baseline: only the delta this probe opens is closed during cleanup.
    const int baseline_size = positionSize(client);

    int opened_size = 0;
    auto cleanup = [&]()
    {
        if (0 == opened_size)
        {
            return;
        }
        nlohmann::json close_body{
            { "contract", kContract },
            { "size", -opened_size },
            { "price", "0" },
            { "tif", "ioc" },
            { "reduce_only", true },
        };
        auto closed = client.postFuturesOrder(close_body);
        if (!ok(closed))
        {
            std::cout << "[WARN] cleanup close failed: " << error(closed).message
                      << " — close manually on testnet\n";
        }
        else
        {
            std::cout << "[OK]   cleanup: closed " << opened_size << " contract(s)\n";
            opened_size = 0;
        }
    };

    // 3. Open a 1-contract market long (size positive = buy).
    nlohmann::json open_body{
        { "contract", kContract },
        { "size", 1 },
        { "price", "0" },
        { "tif", "ioc" },
        { "reduce_only", false },
    };
    auto opened = client.postFuturesOrder(open_body);
    if (!ok(opened))
    {
        return fail("open market order", error(opened));
    }
    std::cout << "[OK]   open order accepted: " << value(opened).dump().substr(0, 160) << "\n";

    // Give the matching engine a moment, then read the actual position.
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto positions = client.getFuturesPositions();
    if (!ok(positions))
    {
        return fail("futures positions", error(positions));
    }
    double entry = 0.0;
    int position_size = 0;
    for (const auto &p : value(positions))
    {
        if (kContract != p.value("contract", ""))
        {
            continue;
        }
        position_size = p.value("size", 0);
        entry = std::stod(p.value("entry_price", "0"));
    }
    opened_size = position_size - baseline_size;
    if (opened_size <= 0)
    {
        std::cout << "[FAIL] position delta is " << opened_size
                  << " (size=" << position_size << ", baseline=" << baseline_size
                  << ") — nothing opened by this probe\n";
        return 1;
    }
    std::cout << "[OK]   position: size=" << position_size << " entry=" << entry
              << " (opened " << opened_size << ")\n";

    // 4. Protective stop — the exact body the engine builds.
    //
    // The trigger is derived from the LAST price rather than the entry: the
    // testnet position may already be far underwater, and Gate rejects a long
    // stop at/above last (1029 AUTO_TRIGGER_PRICE_LESS_LAST). The body shape
    // is identical to the engine's.
    auto ticker = client.getFuturesTicker(kContract);
    if (!ok(ticker))
    {
        const int rc = fail("futures ticker", error(ticker));
        cleanup();
        return rc;
    }
    double last = 0.0;
    const auto &ticker_json = value(ticker);
    try
    {
        // /futures/usdt/tickers?contract=... returns an array.
        if (ticker_json.is_array() && !ticker_json.empty())
        {
            last = std::stod(ticker_json[0].value("last", "0"));
        }
        else
        {
            last = std::stod(ticker_json.value("last", "0"));
        }
    }
    catch (...)
    {
        last = 0.0;
    }
    if (last <= 0.0)
    {
        std::cout << "[FAIL] no last price for " << kContract << "\n";
        cleanup();
        return 1;
    }

    constexpr double kStopPct = 0.01;
    const double trigger = std::round(last * (1.0 - kStopPct) / price_tick) * price_tick;
    std::cout << "[INFO] last=" << last << " trigger=" << trigger << "\n";
    nlohmann::json stop_body{
        { "initial", nlohmann::json{
              { "contract", kContract },
              { "size", 0 },
              { "price", "0" },
              { "tif", "ioc" },
              { "is_reduce_only", true },
              { "auto_size", "close" },
          } },
        { "trigger", nlohmann::json{
              { "price", std::to_string(trigger) },
              { "rule", 2 },        // price <= trigger closes the long
              { "price_type", 0 },
              { "expiration", 0 },
          } },
        { "order_type", "close-long-position" },
    };

    auto stop = client.postFuturesPriceOrder(stop_body);
    if (!ok(stop))
    {
        std::cout << "[FAIL] protective stop rejected — body: " << stop_body.dump() << "\n";
        const int rc = fail("price_orders", error(stop));
        cleanup();
        return rc;
    }
    std::cout << "[OK]   protective stop accepted: " << value(stop).dump().substr(0, 160) << "\n";

    // 5. Confirm it is listed as an open trigger.
    auto triggers = client.getFuturesPriceOrders(kContract);
    if (!ok(triggers))
    {
        const int rc = fail("list price orders", error(triggers));
        cleanup();
        return rc;
    }
    // Use the id echoed by the POST — the open list may also contain stale
    // triggers from earlier probes (verified 2026-11-13: a list-first pick
    // cancelled the wrong id).
    const std::string trigger_id =
        std::to_string(value(stop).contains("id")
                           ? value(stop)["id"].get<std::int64_t>()
                           : 0);
    bool listed = false;
    for (const auto &t : value(triggers))
    {
        if (t.contains("id") && trigger_id == std::to_string(t["id"].get<std::int64_t>()))
        {
            listed = true;
            break;
        }
    }
    if ("0" == trigger_id || !listed)
    {
        std::cout << "[FAIL] protective stop not present in the open trigger list "
                     "(id=" << trigger_id << ")\n";
        cleanup();
        return 1;
    }
    std::cout << "[OK]   trigger listed: id=" << trigger_id << "\n";

    // 6. Cleanup — cancel the trigger, then close the opened delta.
    auto cancelled = client.cancelFuturesPriceOrder(trigger_id);
    if (!ok(cancelled))
    {
        std::cout << "[WARN] cancel trigger failed: " << error(cancelled).message << "\n";
    }
    else
    {
        std::cout << "[OK]   trigger cancelled\n";
    }

    cleanup();
    if (0 != opened_size)
    {
        return 1;
    }

    std::cout << "=== PASS ===\n";
    return 0;
}