// test_control_auth.cpp — Token authentication on the control socket (M-Q4)
//
// Covers the transport-level gate: when [control] token is set, exactly one
// `auth` line is accepted before any other request, comparison happens on
// SHA-256 digests, and a rejected connection is answered with -32603 carrying
// PulseError 9106 and then closed.
//
// Two layers are exercised:
//   - ControlClient end to end (accepted, wrong token, missing token)
//   - raw POSIX sockets for the wire shape, which the client hides

#include "control/ControlClient.hpp"
#include "control/JsonRpcServer.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>

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
    return reg;
}

JsonRpcServer::Security tokenSecurity(const std::string &token)
{
    JsonRpcServer::Security security;
    security.token = token;
    return security;
}

/// Connect a blocking POSIX socket to the loopback control port.
int connectRaw(std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (0 != ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Read one newline-terminated line, or "" when the peer closed.
std::string readLineRaw(int fd)
{
    std::string line;
    char ch = 0;
    while (true)
    {
        const ssize_t n = ::recv(fd, &ch, 1, 0);
        if (n <= 0)
        {
            return line;
        }
        if ('\n' == ch)
        {
            return line;
        }
        line.push_back(ch);
    }
}

void sendLineRaw(int fd, const std::string &line)
{
    const std::string payload = line + "\n";
    ::send(fd, payload.data(), payload.size(), 0);
}

} // anonymous namespace

TEST(ControlAuth, NoTokenServerAllowsPlainCall)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    ControlClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", server->port(), 3000));

    auto result = client.call("echo", nlohmann::json{ { "x", 1 } });
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_EQ(1, value(result)["x"]);

    server->stop();
}

TEST(ControlAuth, CorrectTokenIsAccepted)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    server->setSecurity(tokenSecurity("s3cret"));
    ASSERT_TRUE(server->start());

    ControlClient client;
    client.setToken("s3cret");
    ASSERT_TRUE(client.connect("127.0.0.1", server->port(), 3000));

    auto result = client.call("echo", nlohmann::json{ { "ok", true } });
    ASSERT_TRUE(ok(result)) << error(result).message;
    EXPECT_TRUE(value(result)["ok"]);

    server->stop();
}

TEST(ControlAuth, WrongTokenIsRejected)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    server->setSecurity(tokenSecurity("s3cret"));
    ASSERT_TRUE(server->start());

    ControlClient client;
    client.setToken("guess");
    ASSERT_TRUE(client.connect("127.0.0.1", server->port(), 3000));

    auto result = client.call("echo");
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlAuthFailed, error(result).code);

    server->stop();
}

TEST(ControlAuth, MissingTokenIsRejected)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    server->setSecurity(tokenSecurity("s3cret"));
    ASSERT_TRUE(server->start());

    // No token configured on the client: the first real call must come back as
    // an authentication failure, not as "engine unreachable".
    ControlClient client;
    ASSERT_TRUE(client.connect("127.0.0.1", server->port(), 3000));

    auto result = client.call("echo");
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlAuthFailed, error(result).code);

    server->stop();
}

TEST(ControlAuth, ClientTokenAgainstOpenServerFails)
{
    // A security downgrade must be explicit: the client refuses to continue
    // when the server has no auth to negotiate.
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    ControlClient client;
    client.setToken("s3cret");
    ASSERT_TRUE(client.connect("127.0.0.1", server->port(), 3000));

    auto result = client.call("echo");
    ASSERT_FALSE(ok(result));
    EXPECT_EQ(ErrorCode::ControlAuthFailed, error(result).code);

    server->stop();
}

TEST(ControlAuth, FirstMethodMustBeAuth)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    server->setSecurity(tokenSecurity("s3cret"));
    ASSERT_TRUE(server->start());

    const int fd = connectRaw(server->port());
    ASSERT_GE(fd, 0);

    // A normal request before authenticating is refused with 9106.
    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":7,"method":"echo","params":{}})");
    const std::string reply = readLineRaw(fd);
    ::close(fd);

    ASSERT_FALSE(reply.empty());
    const auto parsed = nlohmann::json::parse(reply);
    ASSERT_TRUE(parsed.contains("error"));
    EXPECT_EQ(-32603, parsed["error"].value("code", 0));
    EXPECT_EQ(static_cast<int>(ErrorCode::ControlAuthFailed),
              parsed["error"]["data"].value("code", 0));
    EXPECT_EQ(7, parsed["id"].get<int>());

    server->stop();
}

TEST(ControlAuth, AuthReplyEchoesIdAndOpensTheSession)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    server->setSecurity(tokenSecurity("s3cret"));
    ASSERT_TRUE(server->start());

    const int fd = connectRaw(server->port());
    ASSERT_GE(fd, 0);

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":3,"method":"auth","params":{"token":"s3cret"}})");
    const std::string authReply = readLineRaw(fd);

    ASSERT_FALSE(authReply.empty());
    const auto parsedAuth = nlohmann::json::parse(authReply);
    EXPECT_EQ(3, parsedAuth["id"].get<int>());
    EXPECT_TRUE(parsedAuth["result"].value("authenticated", false));

    // The same connection now carries normal requests.
    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":4,"method":"echo","params":{"after":true}})");
    const std::string echoReply = readLineRaw(fd);
    ::close(fd);

    ASSERT_FALSE(echoReply.empty());
    const auto parsedEcho = nlohmann::json::parse(echoReply);
    EXPECT_EQ(4, parsedEcho["id"].get<int>());
    EXPECT_TRUE(parsedEcho["result"].value("after", false));

    server->stop();
}
