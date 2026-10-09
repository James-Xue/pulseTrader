// McpServer.cpp — see McpServer.hpp

#include "control/McpServer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace pulse::control
{

namespace
{

constexpr const char *kServerName = "pulsetrader";
constexpr const char *kServerVersion = "0.1.0";

/// The `_meta` key a MODERN (2026-07-28 and later) request uses to declare the
/// revision governing it. A legacy client states its version inside
/// `initialize` instead, so a version present here IS the era marker.
constexpr const char *kProtocolVersionMetaKey =
    "io.modelcontextprotocol/protocolVersion";

/// The value a modern result must carry. Revision 2026-07-28 requires it on
/// every result, and the "absent means complete" bridge is granted only to
/// earlier-revision servers — a client that saw 2026-07-28 in our version list
/// rejects a result without it, and a rejected tools/list means the model is
/// handed NO TOOLS while the server still looks connected.
constexpr const char *kResultTypeComplete = "complete";

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

/// The capabilities block, byte-identical in `initialize` and
/// `server/discover` because it describes the SERVER rather than a session.
///
/// `listChanged:false` is not a placeholder: the tool catalog is derived from
/// the control-plane method list, so it cannot change while the process runs.
nlohmann::json capabilitiesPayload()
{
    return nlohmann::json{
        { "tools", nlohmann::json{ { "listChanged", false } } },
    };
}

/// Server identity, shaped as the spec's ServerInfo (name + version).
nlohmann::json serverInfoPayload()
{
    return nlohmann::json{
        { "name", kServerName },
        { "version", kServerVersion },
    };
}

/// A JSON-Schema property entry for a string parameter. Required-ness is
/// expressed by the schema-level "required" array, never a per-property
/// boolean (an invalid "required": true is rejected by API schema validators).
nlohmann::json stringParam(const std::string &name,
                           const std::string &description)
{
    return nlohmann::json{
        { "type", "string" },
        { "description", description },
    };
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Protocol revisions
// ---------------------------------------------------------------------------
const std::vector<std::string> &McpServer::legacyProtocolVersions()
{
    // Ascending, newest last. 2024-10-07 is the revision before the first one
    // most clients name; leaving it out means a client asking for it gets
    // counter-offered a version it may not know, and the negotiation then has
    // nowhere to go.
    static const std::vector<std::string> versions{
        "2024-10-07",
        "2024-11-05",
        "2025-03-26",
        "2025-06-18",
        "2025-11-25",
    };
    return versions;
}

const std::string &McpServer::newestLegacyProtocolVersion()
{
    return legacyProtocolVersions().back();
}

const std::vector<std::string> &McpServer::supportedProtocolVersions()
{
    // Modern first, then legacy descending — the order the spec's own example
    // uses (["2026-07-28", "2025-11-25"]), so a client that walks the list and
    // takes the first entry it recognises lands on the newest revision it can
    // speak rather than the oldest.
    static const std::vector<std::string> versions = [] {
        std::vector<std::string> all{ "2026-07-28" };
        const std::vector<std::string> &legacy = legacyProtocolVersions();
        all.insert(all.end(), legacy.rbegin(), legacy.rend());
        return all;
    }();
    return versions;
}

std::string McpServer::negotiateLegacyVersion(const std::string &requested)
{
    const std::vector<std::string> &legacy = legacyProtocolVersions();
    if (std::find(legacy.begin(), legacy.end(), requested) != legacy.end())
    {
        // The spec requires echoing a supported version verbatim; a client that
        // asked for 2025-06-18 and is answered with 2025-11-25 has to decide
        // whether to continue, and the answer is only ever "yes" by luck.
        return requested;
    }
    return newestLegacyProtocolVersion();
}

std::string McpServer::declaredProtocolVersion(const nlohmann::json &params)
{
    if (!params.is_object())
    {
        return std::string{};
    }

    // Note the two shapes a client can use. A string is the documented form; an
    // object is what a few clients send, carrying the revision under a nested
    // key. Both are read because the alternative — treating the object form as
    // "no version declared" — silently downgrades a modern client to the legacy
    // path, which is the failure this whole mechanism exists to prevent.
    const auto meta = params.find("_meta");
    if (params.end() == meta || !meta->is_object())
    {
        return std::string{};
    }

    const auto entry = meta->find(kProtocolVersionMetaKey);
    if (meta->end() == entry)
    {
        return std::string{};
    }
    if (entry->is_string())
    {
        return entry->get<std::string>();
    }

    return std::string{};
}

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

    add("get_signals",
        "Latest per-strategy signals + indicator snapshots + aggregator "
        "consensus from the signal board. Entries older than ~120s should be "
        "treated as stale by consumers.",
        nlohmann::json::object(), {});

    add("grid_start",
        "Start the engine-native futures grid (M27): levels of short limit "
        "orders with per-level reduce-only TP, trend-gated. Refused when "
        "non-grid positions exist on the symbol unless force is configured. "
        "Optional params: levels, qty_per_level, step, anchor.",
        nlohmann::json{ { "levels", nlohmann::json{ { "type", "number" } } },
                        { "qty_per_level", nlohmann::json{ { "type", "number" } } },
                        { "step", nlohmann::json{ { "type", "number" } } },
                        { "anchor", nlohmann::json{ { "type", "number" } } } },
        {});

    add("grid_status",
        "Full grid snapshot: phase, anchor/step/top, per-level state, "
        "realized/unrealized PnL, trend gate, spike/daily-loss freezes.",
        nlohmann::json::object(), {});

    add("grid_pause",
        "Pause the grid: existing orders stay on the exchange, no new "
        "actions until grid_resume/grid_start.",
        nlohmann::json::object(), {});

    add("grid_resume",
        "Resume a paused grid: phase Paused → Running. Orders were kept "
        "during the pause; the next slow pass reconciles with the exchange "
        "before any action.",
        nlohmann::json::object(), {});

    add("get_param_history",
        "Recent strategy parameter changes (newest first): who changed what, "
        "when, from what to what, via which channel (ai/manual).",
        nlohmann::json::object(), {});

    add("get_strategy_performance",
        "Per-strategy trade performance over the last N hours (trades DB): "
        "trade count, total pnl, win rate, fees.",
        nlohmann::json{
            { "hours", nlohmann::json{
                  { "type", "number" },
                  { "description", "Lookback window in hours (default 24)" } } },
        },
        {});

    add("grid_stop",
        "Stop the grid and cancel ALL eth-grid-* orders on the symbol.",
        nlohmann::json::object(), {});

    add("sync_positions",
        "Reconcile the engine position view against the exchange (futures + "
        "CFD): imports missing positions and prunes local ghosts no longer "
        "on the exchange (e.g. positions closed manually in the app). "
        "Runs automatically every ~10s; call this to force it.",
        nlohmann::json::object(), {});

    add("modify_sl_tp",
        "Dynamically adjust the protective stops attached to an open CFD "
        "position (exchange-native price_sl/price_tp, so they survive the "
        "engine dying). Pass 0 to clear a stop; omit a field to keep the "
        "current value. CFD positions only.",
        nlohmann::json{
            { "position_id", stringParam("position_id",
                                         "Position ID (e.g. XAUUSD_Buy_1)") },
            { "sl_price", nlohmann::json{
                  { "type", "number" },
                  { "description", "New stop-loss price (0 = clear)" } } },
            { "tp_price", nlohmann::json{
                  { "type", "number" },
                  { "description", "New take-profit price (0 = clear)" } } },
        },
        { "position_id" });

    add("list_strategies",
        "Registered strategies: id, symbol, enabled, running, paused.",
        nlohmann::json::object(), {});

    add("get_strategy_params",
        "Read one strategy's live params (order_quantity, min_confidence, etc.).",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id",
                                         "Strategy instance ID") },
        },
        { "strategy_id" });

    add("set_strategy_param",
        "Set one strategy param. Valid params: order_quantity, min_confidence, "
        "ema_fast_period, ema_slow_period, bb_period, bb_std_dev, "
        "ob_imbalance_threshold, ob_depth, supertrend_period, "
        "supertrend_multiplier, cooldown_seconds, stop_loss_pct, take_profit_pct.",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id",
                                         "Strategy instance ID") },
            { "param", stringParam("param", "Param name") },
            { "value", nlohmann::json{
                  { "type", "number" },
                  { "description", "New value" } } },
        },
        { "strategy_id", "param", "value" });

    add("open_order",
        "Place an order through the full risk gate "
        "(evaluateOrder → reservation → execute → track).",
        nlohmann::json{
            { "symbol", stringParam("symbol", "Trading pair (e.g. BTC_USDT)") },
            { "side", stringParam("side", "\"buy\" or \"sell\"") },
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
                  { "enum", nlohmann::json::array({ "spot", "futures", "cfd" }) },
                  { "description", "Default: the active trading direction" } } },
            { "leverage", nlohmann::json{
                  { "type", "number" },
                  { "description", "Futures leverage" } } },
            { "reduce_only", nlohmann::json{
                  { "type", "boolean" },
                  { "description", "Reduce-only (futures)" } } },
            { "client_order_id", stringParam("client_order_id",
                                            "Client-assigned order ID") },
            { "sl_price", nlohmann::json{
                  { "type", "number" },
                  { "description",
                    "Attached stop-loss price (CFD only; exchange-native "
                    "protection on the entry order)" } } },
            { "tp_price", nlohmann::json{
                  { "type", "number" },
                  { "description",
                    "Attached take-profit price (CFD only)" } } },
        },
        { "symbol", "side", "quantity" });

    add("close_position",
        "Close an open position via a risk-gated market (or limit) order. "
        "Returns the order id; realized PnL appears via get_orders afterwards.",
        nlohmann::json{
            { "position_id", stringParam("position_id",
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
            { "order_id", stringParam("order_id", "Exchange order ID") },
        },
        { "order_id" });

    add("place_trigger_order",
        "Create a futures trigger order (price_orders) — the TP/SL attached "
        "to a futures position. rule: 1 = price crosses above (SL for shorts "
        "/ TP for longs), 2 = price crosses below (TP for shorts / SL for "
        "longs). WHOLE-POSITION CLOSE ONLY: Gate price_orders requires "
        "auto_size (non-empty) and rejects any non-zero size — for partial "
        "closes (e.g. a grid level's TP) use open_order with reduce_only.",
        nlohmann::json{
            { "contract", stringParam("contract",
                                      "Futures contract (e.g. SNDK_USDT)") },
            { "trigger_price", nlohmann::json{
                  { "type", "number" },
                  { "description", "Trigger price" } } },
            { "rule", nlohmann::json{
                  { "type", "number" },
                  { "description", "1 = price crosses above, 2 = below (default 2)" } } },
            { "size", nlohmann::json{
                  { "type", "number" },
                  { "description", "Must be 0 — whole-position close only" } } },
            { "order_type", stringParam("order_type",
                                        "close-short-position (default) / close-long-position") },
            { "tif", stringParam("tif", "Time in force for the triggered order (default ioc)") },
            { "auto_size", stringParam("auto_size",
                                       "Whole-position close mode: close (default) / close_long / close_short") },
        },
        { "contract", "trigger_price" });

    add("list_trigger_orders",
        "List open futures trigger orders for a contract.",
        nlohmann::json{
            { "contract", stringParam("contract",
                                      "Futures contract (e.g. SNDK_USDT)") },
        },
        { "contract" });

    add("cancel_trigger_order",
        "Cancel a futures trigger order by ID.",
        nlohmann::json{
            { "order_id", stringParam("order_id", "Trigger order ID") },
        },
        { "order_id" });

    add("list_futures_orders",
        "List ALL open futures orders for a contract straight from the "
        "exchange — including orders the engine did not place itself "
        "(App-side orders, pre-restart leftovers) that the tracker-based "
        "get_orders view misses.",
        nlohmann::json{
            { "contract", stringParam("contract",
                                      "Futures contract (e.g. SNDK_USDT)") },
        },
        { "contract" });

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
            { "symbol", stringParam("symbol", "Trading pair (e.g. BTC_USDT)") },
            { "book_levels", nlohmann::json{
                  { "type", "number" },
                  { "description", "Order book depth (default 5)" } } },
            { "klines", nlohmann::json{
                  { "type", "number" },
                  { "description", "Number of klines to return (0 = none)" } } },
            { "market_type", nlohmann::json{
                  { "type", "string" },
                  { "enum", nlohmann::json::array({ "spot", "futures", "cfd" }) } } },
        },
        { "symbol" });

    add("switch_direction",
        "Switch the single active trading direction. Pauses the other "
        "direction's strategies and cancels its open orders; open positions "
        "remain (close manually). Restart returns to config active_market.",
        nlohmann::json{
            { "direction", nlohmann::json{
                  { "type", "string" },
                  { "enum", nlohmann::json::array({ "spot", "futures", "cfd" }) },
                  { "description", "Direction to activate" } } },
        },
        { "direction" });

    add("pause_strategy",
        "Pause a strategy by instance ID (thread stays alive, ticks skipped).",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id",
                                         "Strategy instance ID") },
        },
        { "strategy_id" });

    add("resume_strategy",
        "Resume a paused strategy by instance ID.",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id",
                                         "Strategy instance ID") },
        },
        { "strategy_id" });

    add("set_strategy_trading",
        "One-click per-strategy auto-trade switch. enabled=false: the "
        "strategy's signals still publish to the signal board (get_signals) "
        "but never place orders (signals-only). enabled=true: signals "
        "participate in aggregation and may place orders through the risk "
        "gate. Independent of the active direction (a futures strategy still "
        "needs switch_direction futures to actually trade while cfd is "
        "active). Startup default comes from [strategy] signal_only; a "
        "restart resets the switch. Manual orders (open_order) are "
        "unaffected. Returns the strategy list snapshot.",
        nlohmann::json{
            { "strategy_id", stringParam("strategy_id",
                                         "Strategy instance ID") },
            { "enabled", nlohmann::json{
                  { "type", "boolean" },
                  { "description", "true = auto-trade on; false = signals-only" } } },
        },
        { "strategy_id", "enabled" });

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

    // params is optional in JSON-RPC. An absent (or non-object) key becomes an
    // empty object so every branch below can read it unconditionally, and a
    // non-object value is normalised rather than rejected here because the
    // per-method checks own that verdict.
    const nlohmann::json params =
        (req.contains("params") && req["params"].is_object()) ? req["params"]
                                                             : nlohmann::json::object();

    // Whether this request arrived through the modern (per-request metadata)
    // era. Set at the gate below and read by the respond funnel, so it must be
    // declared before the lambda that captures it.
    bool modern_request = false;

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

        // Modern results must be self-describing. 2026-07-28 requires
        // `resultType` on every result, and the "absent means complete" bridge
        // is granted only to earlier-revision servers. The cost of omitting it
        // is not a warning: a client that saw 2026-07-28 in our version list
        // rejects the result outright, and a rejected tools/list means the
        // model is handed NO TOOLS while the server still reports Connected.
        //
        // Stamped in the one funnel so no future method can forget it, and only
        // for modern requests — a legacy client predates the field.
        if (modern_request && resp.contains("result") && resp["result"].is_object())
        {
            resp["result"]["resultType"] = kResultTypeComplete;
        }
        return resp.dump();
    };

    // --- protocol-version gate (modern era) ---
    // A request that declares a revision in `_meta` and names one this server
    // does not speak is refused with -32022 plus the supported list. That is
    // not a dead end: the spec makes it the negotiation step — the client reads
    // the list, picks a revision both sides speak, and retries.
    //
    // A request declaring nothing (a legacy client, which states its version
    // inside `initialize` instead) sails straight through, which is what lets
    // one dispatch path serve both eras. Because the refusal goes through the
    // funnel, a NOTIFICATION declaring an unsupported revision is dropped in
    // silence rather than provoking an illegal reply.
    const std::string declared = declaredProtocolVersion(params);
    modern_request = !declared.empty();
    if (modern_request)
    {
        const std::vector<std::string> &supported = supportedProtocolVersions();
        if (std::find(supported.begin(), supported.end(), declared) == supported.end())
        {
            return respond(nlohmann::json{
                { "error", nlohmann::json{
                      { "code", kErrorUnsupportedProtocolVersion },
                      { "message", "Unsupported protocol version: " + declared },
                      // Exactly the two keys the client reads. A list under any
                      // other name is unreadable to it, and an unreadable list
                      // makes it abandon negotiation and fall back to
                      // `initialize` with no diagnostic whatsoever.
                      { "data", nlohmann::json{
                            { "supported", supported },
                            { "requested", declared } } } } } });
        }
    }

    // --- server/discover (the modern entry point) ---
    if ("server/discover" == method)
    {
        // ttlMs and cacheScope are stated rather than left to the client's
        // defaults: the reply describes THIS build of THIS server, so caching
        // it would be wrong. `serverInfo` is included by analogy with
        // `initialize`; it lives at the top level here because the modern
        // result schema is a loose object.
        return respond(nlohmann::json{
            { "result", nlohmann::json{
                  { "supportedVersions", supportedProtocolVersions() },
                  { "capabilities", capabilitiesPayload() },
                  { "serverInfo", serverInfoPayload() },
                  { "ttlMs", 0 },
                  { "cacheScope", "private" } } } });
    }

    // --- initialize ---
    if ("initialize" == method)
    {
        // The requested revision lives in `params.protocolVersion` here, NOT in
        // `_meta`: that placement is the whole distinction between the eras. A
        // missing or wrong-typed value is not an error — negotiation can still
        // succeed by counter-offer, and throwing on client input would turn a
        // recoverable mismatch into a crash.
        std::string requested;
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
        {
            requested = params["protocolVersion"].get<std::string>();
        }

        return respond(nlohmann::json{
            { "result", nlohmann::json{
                  { "protocolVersion", negotiateLegacyVersion(requested) },
                  { "capabilities", capabilitiesPayload() },
                  { "serverInfo", serverInfoPayload() } } } });
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
