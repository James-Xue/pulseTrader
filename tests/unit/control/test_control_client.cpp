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

TEST(ControlClient, ReconnectsAfterServerRestart)
{
    // The MCP/CLI bridges hold one long-lived socket. When the engine
    // restarts, that socket dies; the client must transparently reconnect
    // to the remembered endpoint on the next call (2026-08-18 fix — the
    // bridge previously stayed dead with 9104 until manually restarted).
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());
    const auto port = server->port();

    ControlClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 3000));

    auto first = client.call("echo", nlohmann::json{ { "x", 1 } });
    ASSERT_TRUE(ok(first)) << error(first).message;
    EXPECT_EQ(1, value(first)["x"]);

    // Simulate the engine going down: the listening socket closes.
    server->stop();
    server.reset();

    // The stale socket fails with a transport error (9104).
    auto dead = client.call("echo");
    ASSERT_FALSE(ok(dead));
    EXPECT_EQ(ErrorCode::ControlEngineUnreachable, error(dead).code);

    // A new engine binds the same port — the next call must reconnect and
    // succeed without an explicit connect().
    auto server2 = std::make_unique<JsonRpcServer>("127.0.0.1", port, makeRegistry());
    ASSERT_TRUE(server2->start());

    auto revived = client.call("echo", nlohmann::json{ { "x", 2 } });
    ASSERT_TRUE(ok(revived)) << error(revived).message;
    EXPECT_EQ(2, value(revived)["x"]);

    server2->stop();
}
