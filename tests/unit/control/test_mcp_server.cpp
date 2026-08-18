// test_mcp_server.cpp — Unit tests for the stdio MCP server protocol layer

#include "control/McpServer.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace pulse;
using namespace pulse::control;

namespace
{

/// Records the last (method, params) seen by the backend.
struct CallRecorder
{
    std::string method;
    nlohmann::json params;
};

McpServer::Backend makeEchoBackend(CallRecorder &recorder)
{
    return [&](const std::string &method, const nlohmann::json &params)
    {
        recorder.method = method;
        recorder.params = params;
        return RpcResult{ nlohmann::json{
            { "called", method },
            { "params", params },
        } };
    };
}

} // anonymous namespace

TEST(McpServer, InitializeReturnsServerInfo)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        backend, should_exit);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ("2025-06-18", resp["result"]["protocolVersion"]);
    EXPECT_EQ("pulsetrader", resp["result"]["serverInfo"]["name"]);
    EXPECT_TRUE(resp["result"]["capabilities"]["tools"].is_object());
    EXPECT_FALSE(should_exit);
}

TEST(McpServer, InitializedNotificationNoResponse)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
        backend, should_exit);
    EXPECT_TRUE(line.empty());
}

TEST(McpServer, ExitNotificationSetsFlag)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","method":"notifications/exit"})",
        backend, should_exit);
    EXPECT_TRUE(line.empty());
    EXPECT_TRUE(should_exit);
}

TEST(McpServer, PingReturnsEmptyResult)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":9,"method":"ping"})", backend, should_exit);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_TRUE(resp["result"].is_object());
}

TEST(McpServer, ToolsListHasAllTools)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", backend,
        should_exit);
    const auto resp = nlohmann::json::parse(line);
    const auto &tools = resp["result"]["tools"];
    EXPECT_EQ(24, tools.size());
    for (const auto &tool : tools)
    {
        EXPECT_TRUE(tool["name"].is_string());
        EXPECT_TRUE(tool["inputSchema"].is_object());
    }
}

TEST(McpServer, ToolsCallSuccessReturnsTextContent)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_status","arguments":{"a":1}}})",
        backend, should_exit);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ("get_status", rec.method);
    EXPECT_EQ(1, rec.params["a"]);
    EXPECT_FALSE(resp["result"]["isError"].get<bool>());
    EXPECT_EQ("text", resp["result"]["content"][0]["type"]);
}

TEST(McpServer, ToolsCallEngineErrorIsErrorContent)
{
    McpServer::Backend failing_backend =
        [](const std::string &, const nlohmann::json &)
    {
        return RpcResult{ PulseError{ ErrorCode::ControlEngineUnreachable,
                                      "engine down" } };
    };
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"get_status","arguments":{}}})",
        failing_backend, should_exit);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_TRUE(resp["result"]["isError"].get<bool>());
    EXPECT_NE(std::string::npos,
              resp["result"]["content"][0]["text"].get<std::string>().find(
                  "engine down"));
}

TEST(McpServer, ToolsCallUnknownToolIs32602)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"bogus","arguments":{}}})",
        backend, should_exit);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(-32602, resp["error"]["code"]);
}

TEST(McpServer, MalformedLineReturns32700)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine("garbage", backend, should_exit);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(-32700, resp["error"]["code"]);
}

TEST(McpServer, UnknownMethodReturns32601)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":6,"method":"bogus/method"})", backend,
        should_exit);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(-32601, resp["error"]["code"]);
}

TEST(McpServer, ToolDefinitionsAllHaveSchemas)
{
    const auto tools = McpServer::toolDefinitions();
    EXPECT_EQ(24, tools.size());
    bool found_get_signals = false;
    for (const auto &tool : tools)
    {
        const auto &schema = tool["inputSchema"];
        EXPECT_EQ("object", schema["type"]);
        EXPECT_TRUE(schema["properties"].is_object());
        for (const auto &prop : schema["properties"].items())
        {
            // Required-ness lives in the schema-level "required" array only;
            // a per-property boolean "required": true is rejected by API
            // schema validators ("true is not of type array").
            EXPECT_TRUE(prop.value().contains("type"));
            EXPECT_FALSE(prop.value().contains("required"));
        }
        if ("get_signals" == tool["name"])
        {
            found_get_signals = true;
            // No-arg tool: empty properties object.
            EXPECT_EQ(0u, schema["properties"].size());
        }
        if ("open_order" == tool["name"])
        {
            // Attached SL/TP (CFD-only) must be in the schema.
            EXPECT_TRUE(schema["properties"].contains("sl_price"));
            EXPECT_TRUE(schema["properties"].contains("tp_price"));
        }
    }
    EXPECT_TRUE(found_get_signals);
}
