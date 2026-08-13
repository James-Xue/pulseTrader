// test_control_client.cpp — Loopback tests for the control socket client

#include "control/ControlClient.hpp"
#include "control/JsonRpcServer.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace pulse;
using namespace pulse::control;

namespace
{

MethodRegistry makeRegistry()
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
    return reg;
}

} // anonymous namespace

TEST(ControlClient, LoopbackRoundTrip)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    ControlClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", server->port(), 3000));

    auto result = client.call("echo", nlohmann::json{ { "x", 42 } });
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(42, value(result)["x"]);

    server->stop();
    server.reset();
}

TEST(ControlClient, ServerErrorPropagates)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    ControlClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", server->port(), 3000));

    auto result = client.call("fail");
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlProtocolError, error(result).code);

    server->stop();
    server.reset();
}

TEST(ControlClient, ConnectRefusedFails)
{
    ControlClient client;
    // Nothing is listening on this port.
    EXPECT_FALSE(client.connect("127.0.0.1", 1, 500));
}

TEST(ControlClient, CallWhenDisconnectedFails)
{
    ControlClient client;
    auto result = client.call("get_status");
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlEngineUnreachable, error(result).code);
}
