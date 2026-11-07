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
    // This request declares no protocolVersion. The reply used to be a
    // hard-coded "2025-06-18"; it is now a NEGOTIATED counter-offer, and the
    // counter-offer for an undeclared revision is the newest legacy one. That
    // is a deliberate behavioural change: a legacy client can speak any legacy
    // revision, so answering with the newest it understands is strictly better
    // than pinning one at compile time.
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        backend, should_exit);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(McpServer::newestLegacyProtocolVersion(),
              resp["result"]["protocolVersion"].get<std::string>());
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
    EXPECT_EQ(32, tools.size()); // +2: get_param_history / get_strategy_performance
                                 // +1: set_strategy_trading
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
    EXPECT_EQ(32, tools.size()); // +2: get_param_history / get_strategy_performance
                                 // +1: set_strategy_trading
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
        if ("set_strategy_trading" == tool["name"])
        {
            // One-click switch: strategy_id + boolean enabled.
            EXPECT_TRUE(schema["properties"].contains("strategy_id"));
            EXPECT_TRUE(schema["properties"].contains("enabled"));
            EXPECT_EQ("boolean", schema["properties"]["enabled"]["type"]);
        }
    }
    EXPECT_TRUE(found_get_signals);
}


// ---------------------------------------------------------------------------
// Dual-era protocol support (2026-10-09)
//
// MCP revision 2026-07-28 removed the initialize handshake and made the core
// stateless: each request declares its revision in `_meta`, servers must
// implement `server/discover`, and every result must carry `resultType`.
//
// These cases pin the contract that lets ONE dispatch path serve both eras, and
// the three wire details that fail SILENTLY when wrong. A client that cannot
// parse the discover reply or the -32022 data classifies the server as legacy
// and never says why; and a modern result without `resultType` is rejected
// outright, which leaves the model with NO TOOLS while the server still reports
// Connected. That last one is not hypothetical: it is how this was found.
// ---------------------------------------------------------------------------

TEST(McpServerVersions, LegacyListIsTheFiveSdkRevisionsAscendingNewestLast)
{
    const auto &versions = McpServer::legacyProtocolVersions();
    ASSERT_EQ(5u, versions.size());
    EXPECT_EQ("2024-10-07", versions.front());
    EXPECT_EQ("2025-11-25", versions.back());
    for (std::size_t i = 1; i < versions.size(); ++i)
    {
        EXPECT_LT(versions[i - 1], versions[i]) << "the legacy list must ascend";
    }
    EXPECT_EQ("2025-11-25", McpServer::newestLegacyProtocolVersion());
}

TEST(McpServerVersions, SupportedListIsModernFirstThenLegacyDescending)
{
    const auto &versions = McpServer::supportedProtocolVersions();
    ASSERT_EQ(6u, versions.size());
    EXPECT_EQ("2026-07-28", versions.front())
        << "a client takes the first entry it recognises, so newest must lead";
    EXPECT_EQ("2024-10-07", versions.back());
}

TEST(McpServerVersions, NegotiationEchoesSupportedLegacyAndNeverAModernOne)
{
    // A supported legacy revision is echoed VERBATIM: the spec requires it, and
    // a counter-offer to a client that asked for something it knows forces it to
    // decide whether to continue.
    EXPECT_EQ("2025-06-18", McpServer::negotiateLegacyVersion("2025-06-18"));
    EXPECT_EQ("2024-10-07", McpServer::negotiateLegacyVersion("2024-10-07"));

    // Undeclared, unknown, or MODERN: always the newest LEGACY revision.
    // Answering a legacy handshake with a modern revision would hand the client
    // a session shape it cannot parse.
    EXPECT_EQ("2025-11-25", McpServer::negotiateLegacyVersion(""));
    EXPECT_EQ("2025-11-25", McpServer::negotiateLegacyVersion("1999-01-01"));
    EXPECT_EQ("2025-11-25", McpServer::negotiateLegacyVersion("2026-07-28"));
}

TEST(McpServerDualEra, InitializeEchoesTheClientsLegacyVersion)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})",
        backend, should_exit);
    EXPECT_EQ("2025-03-26",
              nlohmann::json::parse(line)["result"]["protocolVersion"].get<std::string>());
}

TEST(McpServerDualEra, DiscoverListsBothErasWithoutAHandshake)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    // Deliberately the FIRST message: no initialize before it.
    const auto resp = nlohmann::json::parse(McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"server/discover","params":{}})",
        backend, should_exit));

    ASSERT_TRUE(resp.contains("result"));
    ASSERT_TRUE(resp["result"].contains("supportedVersions"))
        << "the key is supportedVersions; under any other name a client cannot read it";
    EXPECT_FALSE(resp["result"].contains("protocolVersions"))
        << "the pre-2026 name must NOT also be emitted";

    bool has_modern = false;
    bool has_legacy = false;
    for (const auto &version : resp["result"]["supportedVersions"])
    {
        has_modern = has_modern || ("2026-07-28" == version.get<std::string>());
        has_legacy = has_legacy || ("2025-11-25" == version.get<std::string>());
    }
    EXPECT_TRUE(has_modern);
    EXPECT_TRUE(has_legacy);
    EXPECT_EQ(0, resp["result"]["ttlMs"].get<int>());
    EXPECT_EQ("private", resp["result"]["cacheScope"].get<std::string>());
}

TEST(McpServerDualEra, AModernResultCarriesResultTypeAndALegacyOneDoesNot)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;

    // Modern: `_meta` declares the revision, so resultType is mandatory. Its
    // absence is what a live client rejected with "missing required resultType".
    const auto modern = nlohmann::json::parse(McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
        backend, should_exit));
    ASSERT_TRUE(modern.contains("result"));
    EXPECT_EQ("complete", modern["result"].value("resultType", std::string{}));
    // tools/list is the ONE result whose 2026-07-28 schema also requires these
    // two. A live client rejected the reply with
    //   ttlMs: expected number, received undefined
    //   cacheScope: Invalid option: expected one of "public"|"private"
    // and the model was left with no tools at all — so they are pinned here.
    ASSERT_TRUE(modern["result"].contains("ttlMs"));
    EXPECT_EQ(0, modern["result"]["ttlMs"].get<int>());
    EXPECT_EQ("private", modern["result"]["cacheScope"].get<std::string>());

    // Legacy: nothing declared, so the field predates the client and must be
    // absent rather than described to a client that does not know it.
    const auto legacy = nlohmann::json::parse(McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})",
        backend, should_exit));
    ASSERT_TRUE(legacy.contains("result"));
    EXPECT_FALSE(legacy["result"].contains("resultType"));
    // The catalog must be intact under both eras, stamping or not.
    EXPECT_EQ(McpServer::toolDefinitions().size(), legacy["result"]["tools"].size());
}

TEST(McpServerDualEra, AModernToolsListStillCarriesEveryTool)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto resp = nlohmann::json::parse(McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
        backend, should_exit));
    ASSERT_TRUE(resp["result"].contains("tools"));
    EXPECT_EQ(McpServer::toolDefinitions().size(), resp["result"]["tools"].size())
        << "stamping resultType must not disturb the catalog";
}

TEST(McpServerDualEra, AnUnsupportedDeclaredVersionIsRejectedWithAReadableList)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto resp = nlohmann::json::parse(McpServer::handleLine(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1999-01-01"}}})",
        backend, should_exit));

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(kErrorUnsupportedProtocolVersion, resp["error"]["code"].get<int>());
    // Exactly the two keys the client reads. If `supported` is unreadable it
    // abandons negotiation and silently falls back to the legacy handshake.
    ASSERT_TRUE(resp["error"]["data"].contains("supported"));
    EXPECT_TRUE(resp["error"]["data"]["supported"].is_array());
    EXPECT_EQ("1999-01-01", resp["error"]["data"]["requested"].get<std::string>());
    EXPECT_FALSE(resp["error"]["data"].contains("protocolVersions"));
}

TEST(McpServerDualEra, ANotificationWithAnUnsupportedVersionIsDroppedSilently)
{
    CallRecorder rec;
    auto backend = makeEchoBackend(rec);
    bool should_exit = false;
    const auto line = McpServer::handleLine(
        R"({"jsonrpc":"2.0","method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1999-01-01"}}})",
        backend, should_exit);
    EXPECT_TRUE(line.empty())
        << "a notification must never be answered, not even to refuse it";
}
