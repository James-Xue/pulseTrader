#pragma once
// CommandParser.hpp — REPL command grammar shared by the embedded REPL
// (trade on a TTY) and the remote `cli` subcommand.
//
// Each command line maps to a control-plane JSON-RPC method + params.
// Local commands (help / quit / exit) return nullopt.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace pulse::control
{

struct ParsedCommand
{
    std::string method;
    nlohmann::json params;
};

/// Parse one command line → JSON-RPC method + params.
/// Returns nullopt for local commands (help/quit/exit) or malformed input.
[[nodiscard]] std::optional<ParsedCommand>
parseCommandLine(const std::string &line);

/// True if the line is a local command (help/quit/exit).
[[nodiscard]] bool isLocalCommand(const std::string &line);

/// Pretty-print a method result as human-readable text (tables for
/// positions/orders/strategies, compact JSON otherwise).
[[nodiscard]] std::string formatResponse(const std::string &method,
                                         const nlohmann::json &result);

/// One-line help text for the REPL.
[[nodiscard]] std::string replHelp();

} // namespace pulse::control
