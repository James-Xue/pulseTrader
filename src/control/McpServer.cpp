// McpServer.cpp — see McpServer.hpp

#include "control/McpServer.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace pulse::control
{

namespace
{

constexpr const char *kProtocolVersion = "2025-06-18";
constexpr const char *kServerName = "pulsetrader";
constexpr const char *kServerVersion = "0.1.0";

nlohmann::json makeError(int code, const std::string &message)
{
    return nlohmann::json{
        { "code", code },
        { "message", message },
    };
}

/// Wrap a tool call result as MCP text content.
nlohmann::json textContent(const std::string &text, bool is_error)
{
    return nlohmann::json{
        { "content", nlohmann::json::array({ nlohmann::json{
              { "type", "text" },
              { "text", text } } }) },
        { "isError", is_error },
    };
}

nlohmann::json stringParam(const std::string &name, bool required,
                           const std::string &description)
{
    nlohmann::json p{
        { "type", "string" },
        { "description", description },
    };
    if (required)
    {
        p["required"] = true;
    }
    return p;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Tool definitions
// ---------------------------------------------------------------------------
nlohmann::json McpServer::toolDefinitions()
{
    nlohmann::json defs = nlohmann::json::array();

    auto add = [&defs](const std::string &name, const std::string &description,
                       const nlohmann::json &properties,
                       const std::vector<std::string> &required)
    {
        nlohmann::json schema{
            { "type", "object" },
            { "properties", properties },
        };
        if (!required.empty())
        {
            schema["required"] = required;
        }
        defs.push_back(nlohmann::json{
            { "name", name },
            { "description", description },
            { "inputSchema", schema },
        });
    };

    add("get_status",
        "Engine status: version, uptime, network (mainnet/testnet), symbols, "
        "strategy counts, open positions, trading halted flag, feed stats.",
        nlohmann::json::object(), {});

    add("get_account",
        "Spot + futures account balance (REST).",
        nlohmann::json::object(), {});

    add("get_positions",
        "Open positions with unrealized PnL + portfolio summary.",
        nlohmann::json::object(), {});

    add("get_orders",
        "Active orders + recent execution reports.",
        nlohmann::json::object(), {});

    add("list_strategies",
        "Registered strategies: id, symbol, enabled, running, paused.",
        nlohmann::json::object(), {});

    add("get_strategy_params",
        "Read one strategy's live params (order_quantity, min_confidence, etc.).",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id", true,
                                         "Strategy instance ID") },
        },
        { "strategy_id" });

    add("set_strategy_param",
        "Set one strategy param. Valid params: order_quantity, min_confidence, "
        "ema_fast_period, ema_slow_period, bb_period, bb_std_dev, "
        "ob_imbalance_threshold, ob_depth, supertrend_period, "
        "supertrend_multiplier, cooldown_seconds, stop_loss_pct, take_profit_pct.",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id", true,
                                         "Strategy instance ID") },
            { "param", stringParam("param", true, "Param name") },
            { "value", nlohmann::json{
                  { "type", "number" },
                  { "description", "New value" } } },
        },
        { "strategy_id", "param", "value" });

    add("open_order",
        "Place an order through the full risk gate "
        "(evaluateOrder → reservation → execute → track).",
        nlohmann::json{
            { "symbol", stringParam("symbol", true, "Trading pair (e.g. BTC_USDT)") },
            { "side", stringParam("side", true, "\"buy\" or \"sell\"") },
            { "quantity", nlohmann::json{
                  { "type", "number" },
                  { "description", "Order quantity" } } },
            { "type", nlohmann::json{
                  { "type", "string" },
                  { "enum", nlohmann::json::array({ "market", "limit", "post_only" }) },
                  { "description", "Order type (default market)" } } },
            { "price", nlohmann::json{
                  { "type", "number" },
                  { "description", "Limit price (required for limit/post_only)" } } },
            { "market_type", nlohmann::json{
                  { "type", "string" },
                  { "enum", nlohmann::json::array({ "spot", "futures" }) },
                  { "description", "Default: futures if futures strategies exist" } } },
            { "leverage", nlohmann::json{
                  { "type", "number" },
                  { "description", "Futures leverage" } } },
            { "reduce_only", nlohmann::json{
                  { "type", "boolean" },
                  { "description", "Reduce-only (futures)" } } },
            { "client_order_id", stringParam("client_order_id", false,
                                             "Client-assigned order ID") },
        },
        { "symbol", "side", "quantity" });

    add("close_position",
        "Close an open position via a risk-gated market (or limit) order. "
        "Returns the order id; realized PnL appears via get_orders afterwards.",
        nlohmann::json{
            { "position_id", stringParam("position_id", true,
                                         "Position ID (e.g. BTC_USDT_Buy_1)") },
            { "quantity", nlohmann::json{
                  { "type", "number" },
                  { "description", "Quantity to close (default: full position)" } } },
            { "price", nlohmann::json{
                  { "type", "number" },
                  { "description", "Limit price (market order if omitted)" } } },
        },
        { "position_id" });

    add("cancel_order",
        "Cancel an open order by exchange order ID.",
        nlohmann::json{
            { "order_id", stringParam("order_id", true, "Exchange order ID") },
        },
        { "order_id" });

    add("halt_trading",
        "Manually halt all trading (circuit-breaker override). Returns risk snapshot.",
        nlohmann::json::object(), {});

    add("resume_trading",
        "Resume trading after a halt. Returns risk snapshot.",
        nlohmann::json::object(), {});

    add("get_risk",
        "Risk snapshot: halted flag, drawdown, rate limiter tokens, portfolio.",
        nlohmann::json::object(), {});

    add("get_market",
        "Live market data for a symbol: ticker, top-of-book, klines.",
        nlohmann::json{
            { "symbol", stringParam("symbol", true, "Trading pair (e.g. BTC_USDT)") },
            { "book_levels", nlohmann::json{
                  { "type", "number" },
                  { "description", "Order book depth (default 5)" } } },
            { "klines", nlohmann::json{
                  { "type", "number" },
                  { "description", "Number of klines to return (0 = none)" } } },
            { "market_type", nlohmann::json{
                  { "type", "string" },
                  { "enum", nlohmann::json::array({ "spot", "futures" }) } } },
        },
        { "symbol" });

    add("pause_strategy",
        "Pause a strategy by instance ID (thread stays alive, ticks skipped).",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id", true,
                                         "Strategy instance ID") },
        },
        { "strategy_id" });

    add("resume_strategy",
        "Resume a paused strategy by instance ID.",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id", true,
                                         "Strategy instance ID") },
        },
        { "strategy_id" });

    return defs;
}

// ---------------------------------------------------------------------------
// Line handling
// ---------------------------------------------------------------------------
std::string McpServer::handleLine(const std::string &line, Backend &backend,
                                  bool &should_exit)
{
    nlohmann::json req;
    try
    {
        req = nlohmann::json::parse(line);
    }
    catch (const nlohmann::json::parse_error &)
    {
        return nlohmann::json{
            { "jsonrpc", "2.0" },
            { "id", nullptr },
            { "error", makeError(-32700, "Parse error") },
        }.dump();
    }

    if (!req.is_object() || !req.contains("method")
        || !req["method"].is_string())
    {
        return nlohmann::json{
            { "jsonrpc", "2.0" },
            { "id", nullptr },
            { "error", makeError(-32600, "Invalid request") },
        }.dump();
    }

    const std::string method = req["method"].get<std::string>();
    const bool is_notification = !req.contains("id") || req["id"].is_null();

    auto respond = [&](const nlohmann::json &payload)
    {
        if (is_notification)
        {
            return std::string{};
        }
        nlohmann::json resp{
            { "jsonrpc", "2.0" },
            { "id", req["id"] },
        };
        resp.update(payload);
        return resp.dump();
    };

    // --- initialize ---
    if ("initialize" == method)
    {
        return respond(nlohmann::json{
            { "result", nlohmann::json{
                  { "protocolVersion", kProtocolVersion },
                  { "capabilities", nlohmann::json{
                        { "tools", nlohmann::json{ { "listChanged", false } } } } },
                  { "serverInfo", nlohmann::json{
                        { "name", kServerName },
                        { "version", kServerVersion } } } } } });
    }

    // --- notifications ---
    if ("notifications/initialized" == method)
    {
        return ""; // no response to notifications
    }
    if ("notifications/exit" == method)
    {
        should_exit = true;
        return "";
    }

    // --- ping ---
    if ("ping" == method)
    {
        return respond(nlohmann::json{ { "result", nlohmann::json::object() } });
    }

    // --- tools/list ---
    if ("tools/list" == method)
    {
        return respond(nlohmann::json{
            { "result", nlohmann::json{ { "tools", toolDefinitions() } } } });
    }

    // --- tools/call ---
    if ("tools/call" == method)
    {
        if (!req.contains("params") || !req["params"].is_object()
            || !req["params"].contains("name")
            || !req["params"]["name"].is_string())
        {
            return respond(nlohmann::json{
                { "error", makeError(-32602, "Invalid params: name required") } });
        }

        const std::string name = req["params"]["name"].get<std::string>();
        const nlohmann::json arguments =
            req["params"].value("arguments", nlohmann::json::object());

        // Validate against the tool list.
        bool known = false;
        for (const auto &tool : toolDefinitions())
        {
            if (tool.value("name", "") == name)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            return respond(nlohmann::json{
                { "error", makeError(-32602, "Unknown tool: " + name) } });
        }

        RpcResult result = backend(name, arguments);
        if (ok(result))
        {
            return respond(nlohmann::json{
                { "result", textContent(value(result).dump(2), false) } });
        }
        const auto &err = error(result);
        return respond(nlohmann::json{
            { "result", textContent(
                  "error (code=" + std::to_string(static_cast<int>(err.code))
                      + "): " + err.message,
                  true) } });
    }

    // --- unknown method ---
    return respond(nlohmann::json{
        { "error", makeError(-32601, "Method not found") } });
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------
void McpServer::run(std::istream &in, std::ostream &out)
{
    bool should_exit = false;
    std::string line;
    while (!should_exit && std::getline(in, line))
    {
        if (line.empty())
        {
            continue;
        }
        const std::string response = handleLine(line, m_backend, should_exit);
        if (!response.empty())
        {
            out << response << "\n";
            out.flush();
        }
    }
}

} // namespace pulse::control
