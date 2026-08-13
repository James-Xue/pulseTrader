// CommandParser.cpp — see CommandParser.hpp

#include "control/CommandParser.hpp"

#include "core/types.hpp"

#include <iomanip>
#include <sstream>
#include <vector>

namespace pulse::control
{

namespace
{

/// Split a line into whitespace-separated tokens.
std::vector<std::string> tokenize(const std::string &line)
{
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok)
    {
        tokens.push_back(tok);
    }
    return tokens;
}

/// Parse a double token via safeParseDouble (non-throwing).
std::optional<double> parseNumber(const std::string &tok)
{
    return safeParseDouble(tok);
}

} // anonymous namespace

bool isLocalCommand(const std::string &line)
{
    const auto tokens = tokenize(line);
    if (tokens.empty())
    {
        return true; // empty line
    }
    return "help" == tokens[0] || "quit" == tokens[0] || "exit" == tokens[0]
           || "?" == tokens[0];
}

std::optional<ParsedCommand> parseCommandLine(const std::string &line)
{
    const auto tokens = tokenize(line);
    if (tokens.empty())
    {
        return std::nullopt;
    }

    const std::string &cmd = tokens[0];

    // --- No-arg commands ---
    if ("status" == cmd)
    {
        return ParsedCommand{ "get_status", nlohmann::json::object() };
    }
    if ("account" == cmd || "balance" == cmd)
    {
        return ParsedCommand{ "get_account", nlohmann::json::object() };
    }
    if ("positions" == cmd)
    {
        return ParsedCommand{ "get_positions", nlohmann::json::object() };
    }
    if ("orders" == cmd)
    {
        return ParsedCommand{ "get_orders", nlohmann::json::object() };
    }
    if ("strategies" == cmd)
    {
        return ParsedCommand{ "list_strategies", nlohmann::json::object() };
    }
    if ("halt" == cmd)
    {
        return ParsedCommand{ "halt_trading", nlohmann::json::object() };
    }
    if ("resume" == cmd)
    {
        return ParsedCommand{ "resume_trading", nlohmann::json::object() };
    }
    if ("risk" == cmd)
    {
        return ParsedCommand{ "get_risk", nlohmann::json::object() };
    }

    // --- One-arg commands ---
    if ("params" == cmd && tokens.size() >= 2)
    {
        return ParsedCommand{ "get_strategy_params",
                              nlohmann::json{ { "strategy_id", tokens[1] } } };
    }
    if ("pause" == cmd && tokens.size() >= 2)
    {
        return ParsedCommand{ "pause_strategy",
                              nlohmann::json{ { "strategy_id", tokens[1] } } };
    }
    if ("resume-strategy" == cmd && tokens.size() >= 2)
    {
        return ParsedCommand{ "resume_strategy",
                              nlohmann::json{ { "strategy_id", tokens[1] } } };
    }
    if ("cancel" == cmd && tokens.size() >= 2)
    {
        return ParsedCommand{ "cancel_order",
                              nlohmann::json{ { "order_id", tokens[1] } } };
    }

    // --- set <id> <param> <value> ---
    if ("set" == cmd && tokens.size() >= 4)
    {
        auto value = parseNumber(tokens[3]);
        if (!value.has_value())
        {
            return std::nullopt;
        }
        return ParsedCommand{
            "set_strategy_param",
            nlohmann::json{ { "strategy_id", tokens[1] },
                            { "param", tokens[2] },
                            { "value", *value } }
        };
    }

    // --- close <position_id> [qty] [price] ---
    if ("close" == cmd && tokens.size() >= 2)
    {
        nlohmann::json params{ { "position_id", tokens[1] } };
        if (tokens.size() >= 3)
        {
            auto qty = parseNumber(tokens[2]);
            if (!qty.has_value())
            {
                return std::nullopt;
            }
            params["quantity"] = *qty;
        }
        if (tokens.size() >= 4)
        {
            auto price = parseNumber(tokens[3]);
            if (!price.has_value())
            {
                return std::nullopt;
            }
            params["price"] = *price;
        }
        return ParsedCommand{ "close_position", params };
    }

    // --- open <symbol> <buy|sell> <qty> [flags...] ---
    if ("open" == cmd && tokens.size() >= 4)
    {
        nlohmann::json params{
            { "symbol", tokens[1] },
            { "side", tokens[2] },
        };
        auto qty = parseNumber(tokens[3]);
        if (!qty.has_value())
        {
            return std::nullopt;
        }
        params["quantity"] = *qty;

        // Flags: --type market|limit|post_only, --price P, --market spot|futures,
        //        --leverage N, --reduce-only, --client-id S
        for (std::size_t i = 4; i < tokens.size(); ++i)
        {
            const std::string &flag = tokens[i];
            if ("--type" == flag && i + 1 < tokens.size())
            {
                params["type"] = tokens[++i];
            }
            else if ("--price" == flag && i + 1 < tokens.size())
            {
                auto p = parseNumber(tokens[++i]);
                if (!p.has_value())
                {
                    return std::nullopt;
                }
                params["price"] = *p;
            }
            else if ("--market" == flag && i + 1 < tokens.size())
            {
                params["market_type"] = tokens[++i];
            }
            else if ("--leverage" == flag && i + 1 < tokens.size())
            {
                auto lev = parseNumber(tokens[++i]);
                if (!lev.has_value())
                {
                    return std::nullopt;
                }
                params["leverage"] = *lev;
            }
            else if ("--reduce-only" == flag)
            {
                params["reduce_only"] = true;
            }
            else if ("--client-id" == flag && i + 1 < tokens.size())
            {
                params["client_order_id"] = tokens[++i];
            }
            else
            {
                return std::nullopt; // unknown flag
            }
        }
        return ParsedCommand{ "open_order", params };
    }

    // --- market <symbol> [--levels N] [--klines N] [--market spot|futures] ---
    if ("market" == cmd && tokens.size() >= 2)
    {
        nlohmann::json params{ { "symbol", tokens[1] } };
        for (std::size_t i = 2; i < tokens.size(); ++i)
        {
            const std::string &flag = tokens[i];
            if ("--levels" == flag && i + 1 < tokens.size())
            {
                auto n = parseNumber(tokens[++i]);
                if (!n.has_value())
                {
                    return std::nullopt;
                }
                params["book_levels"] = static_cast<int>(*n);
            }
            else if ("--klines" == flag && i + 1 < tokens.size())
            {
                auto n = parseNumber(tokens[++i]);
                if (!n.has_value())
                {
                    return std::nullopt;
                }
                params["klines"] = static_cast<int>(*n);
            }
            else if ("--market" == flag && i + 1 < tokens.size())
            {
                params["market_type"] = tokens[++i];
            }
            else
            {
                return std::nullopt;
            }
        }
        return ParsedCommand{ "get_market", params };
    }

    return std::nullopt; // unknown command
}

// ---------------------------------------------------------------------------
// Pretty printing
// ---------------------------------------------------------------------------
namespace
{

std::string renderTable(const std::vector<std::vector<std::string>> &rows,
                        const std::vector<std::string> &headers)
{
    if (rows.empty())
    {
        return "(empty)";
    }
    std::vector<std::size_t> widths(headers.size(), 0);
    auto measure = [&](const std::vector<std::string> &cells)
    {
        for (std::size_t i = 0; i < cells.size() && i < widths.size(); ++i)
        {
            widths[i] = std::max(widths[i], cells[i].size());
        }
    };
    measure(headers);
    for (const auto &row : rows)
    {
        measure(row);
    }

    std::ostringstream out;
    auto render = [&](const std::vector<std::string> &cells)
    {
        for (std::size_t i = 0; i < cells.size() && i < widths.size(); ++i)
        {
            out << " " << std::left << std::setw(static_cast<int>(widths[i]))
                << cells[i];
        }
        out << "\n";
    };
    render(headers);
    for (std::size_t i = 0; i < headers.size(); ++i)
    {
        out << " " << std::string(widths[i], '-');
    }
    out << "\n";
    for (const auto &row : rows)
    {
        render(row);
    }
    return out.str();
}

std::string formatPositions(const nlohmann::json &result)
{
    const auto &positions = result.value("positions", nlohmann::json::array());
    std::vector<std::vector<std::string>> rows;
    for (const auto &p : positions)
    {
        rows.push_back({
            p.value("position_id", ""),
            p.value("symbol", ""),
            p.value("side", ""),
            p.value("quantity", 0.0) == 0.0
                ? "0"
                : std::to_string(p.value("quantity", 0.0)),
            std::to_string(p.value("entry_price", 0.0)),
            std::to_string(p.value("unrealized_pnl", 0.0)),
        });
    }
    std::string out = renderTable(
        rows, { "POSITION", "SYMBOL", "SIDE", "QTY", "ENTRY", "PNL" });
    if (result.contains("portfolio"))
    {
        const auto &pf = result["portfolio"];
        out += "open=" + std::to_string(pf.value("openPositionCount", 0))
             + " notional=" + std::to_string(pf.value("total_notional", 0.0))
             + " unreal_pnl=" + std::to_string(pf.value("total_unrealized_pnl", 0.0))
             + "\n";
    }
    return out;
}

std::string formatOrders(const nlohmann::json &result)
{
    const auto &orders = result.value("activeOrders", nlohmann::json::array());
    std::vector<std::vector<std::string>> rows;
    for (const auto &o : orders)
    {
        rows.push_back({
            o.value("order_id", ""),
            o.value("symbol", ""),
            o.value("side", ""),
            o.value("type", ""),
            std::to_string(o.value("requested_qty", 0.0)),
            o.value("status", ""),
        });
    }
    std::string out = renderTable(
        rows, { "ORDER", "SYMBOL", "SIDE", "TYPE", "QTY", "STATUS" });
    const auto &reports = result.value("recentReports", nlohmann::json::array());
    if (!reports.empty())
    {
        out += "recent reports: " + std::to_string(reports.size()) + "\n";
    }
    return out;
}

std::string formatStrategies(const nlohmann::json &result)
{
    std::vector<std::vector<std::string>> rows;
    for (const auto &s : result)
    {
        rows.push_back({
            s.value("id", ""),
            s.value("symbol", ""),
            s.value("enabled", false) ? "on" : "off",
            s.value("running", false) ? "run" : "stop",
            s.value("paused", false) ? "PAUSED" : "-",
        });
    }
    return renderTable(rows, { "STRATEGY", "SYMBOL", "ENABLED", "STATE", "PAUSE" });
}

} // anonymous namespace

std::string formatResponse(const std::string &method,
                           const nlohmann::json &result)
{
    if ("get_positions" == method)
    {
        return formatPositions(result);
    }
    if ("get_orders" == method)
    {
        return formatOrders(result);
    }
    if ("list_strategies" == method || "pause_strategy" == method
        || "resume_strategy" == method)
    {
        return formatStrategies(result);
    }
    return result.dump(2);
}

std::string replHelp()
{
    return
        "Commands:\n"
        "  status                 engine status (uptime, feeds, halted)\n"
        "  account                spot + futures balance\n"
        "  positions              open positions + portfolio\n"
        "  orders                 active orders + recent reports\n"
        "  strategies             registered strategies\n"
        "  params <id>            strategy params\n"
        "  set <id> <param> <v>   set strategy param (e.g. set mom min_confidence 0.7)\n"
        "  open <sym> <buy|sell> <qty> [--type market|limit|post_only]\n"
        "                          [--price P] [--market spot|futures]\n"
        "                          [--leverage N] [--reduce-only] [--client-id S]\n"
        "  close <position_id> [qty] [price]\n"
        "  cancel <order_id>      cancel an open order\n"
        "  halt / resume          halt or resume all trading\n"
        "  pause <id> / resume-strategy <id>\n"
        "  risk                   risk snapshot (drawdown, rate limiter)\n"
        "  market <sym> [--levels N] [--klines N] [--market spot|futures]\n"
        "  help / quit / exit\n";
}

} // namespace pulse::control
