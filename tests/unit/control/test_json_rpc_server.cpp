// test_json_rpc_server.cpp — Unit tests for the JSON-RPC dispatch layer

#include "control/JsonRpcServer.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace pulse;
using namespace pulse::control;

namespace
{

MethodRegistry makeTestRegistry()
{
    MethodRegistry reg;
    reg["echo"] = [](const nlohmann::json &params)
    {
        return RpcResult{ params };
    };
    reg["fail"] = [](const nlohmann::json &)
    {
        return RpcResult{ PulseError{ ErrorCode::OrderRejected, "nope" } };
    };
    reg["thrower"] = [](const nlohmann::json &) -> RpcResult
    {
        throw std::runtime_error("boom");
    };
    return reg;
}

} // anonymous namespace

TEST(JsonRpcServer, ValidRequestEchoesResult)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":1,"method":"echo","params":{"a":1}})", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ("2.0", resp["jsonrpc"]);
    EXPECT_EQ(1, resp["id"]);
    EXPECT_EQ(1, resp["result"]["a"]);
    EXPECT_FALSE(resp.contains("error"));
}

TEST(JsonRpcServer, EchoesStringId)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":"abc","method":"echo","params":{}})", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ("abc", resp["id"]);
}

TEST(JsonRpcServer, NotificationGetsNoResponse)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","method":"echo","params":{}})", reg);
    EXPECT_TRUE(line.empty());
}

TEST(JsonRpcServer, NullIdIsNotification)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":null,"method":"echo","params":{}})", reg);
    EXPECT_TRUE(line.empty());
}

TEST(JsonRpcServer, UnknownMethodReturns32601)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":1,"method":"nope","params":{}})", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(-32601, resp["error"]["code"]);
}

TEST(JsonRpcServer, ParseErrorReturns32700)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine("{not json", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(-32700, resp["error"]["code"]);
    EXPECT_TRUE(resp["id"].is_null());
}

TEST(JsonRpcServer, MissingMethodReturns32600)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(R"({"jsonrpc":"2.0","id":1})", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(-32600, resp["error"]["code"]);
}

TEST(JsonRpcServer, HandlerErrorReturns32603WithDataCode)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":5,"method":"fail","params":{}})", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(-32603, resp["error"]["code"]);
    EXPECT_EQ(static_cast<int>(ErrorCode::OrderRejected),
              resp["error"]["data"]["code"]);
}

TEST(JsonRpcServer, HandlerExceptionReturns32603)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":1,"method":"thrower","params":{}})", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(-32603, resp["error"]["code"]);
}

TEST(JsonRpcServer, ParamsPassedThrough)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":1,"method":"echo","params":{"x":[1,2,3]}})", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(3, resp["result"]["x"].size());
}

TEST(JsonRpcServer, MissingParamsDefaultsToObject)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":1,"method":"echo"})", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_TRUE(resp["result"].is_object());
}

TEST(JsonRpcServer, TrailingCarriageReturnHandled)
{
    const auto reg = makeTestRegistry();
    const auto line = JsonRpcServer::dispatchLine(
        R"({"jsonrpc":"2.0","id":1,"method":"echo","params":{}})" "\r", reg);
    const auto resp = nlohmann::json::parse(line);
    EXPECT_EQ(1, resp["id"]);
}
