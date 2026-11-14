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

// Raw sockets observe the wire shape the client hides. POSIX and Winsock
// differ in header names, handle type (int vs SOCKET) and the close call, so
// the split is contained in the helpers below.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <memory>
#include <string>

using namespace pulse;
using namespace pulse::control;

namespace
{

#ifdef _WIN32
/// Winsock handle; must be released with closesocket(), not close().
using RawSocket = SOCKET;
constexpr RawSocket kInvalidRawSocket = INVALID_SOCKET;

/// asio initialises Winsock lazily and every test builds a JsonRpcServer
/// first, but connectRaw() should not depend on that ordering. Take a
/// process-lifetime reference and never call WSACleanup: asio still owns its
/// own reference, and a cleanup here would tear the socket library out from
/// under it.
void ensureWinsock() noexcept
{
    static const int init = []
    {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    (void)init;
}
#else
using RawSocket = int;
constexpr RawSocket kInvalidRawSocket = -1;

/// POSIX needs no explicit socket-library initialisation.
void ensureWinsock() noexcept
{
}
#endif

/// Close a raw socket with the platform's spelling.
void closeRaw(RawSocket sock) noexcept
{
#ifdef _WIN32
    ::closesocket(sock);
#else
    ::close(sock);
#endif
}

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

/// Connect a blocking raw socket to the loopback control port.
RawSocket connectRaw(std::uint16_t port)
{
    ensureWinsock();
    // Never compare a Winsock SOCKET against 0: it is unsigned, and failure is
    // INVALID_SOCKET rather than a negative descriptor.
    const RawSocket fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (kInvalidRawSocket == fd)
    {
        return kInvalidRawSocket;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (0 != ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
    {
        closeRaw(fd);
        return kInvalidRawSocket;
    }
    return fd;
}

/// Read one newline-terminated line, or "" when the peer closed.
std::string readLineRaw(RawSocket fd)
{
    std::string line;
    char ch = 0;
    while (true)
    {
        // recv() returns ssize_t on POSIX and int on Windows; a one-byte read
        // cannot truncate either way.
        const int n = static_cast<int>(::recv(fd, &ch, 1, 0));
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

void sendLineRaw(RawSocket fd, const std::string &line)
{
    const std::string payload = line + "\n";
    // Winsock's send() takes an int length; POSIX takes size_t. The cast is a
    // no-op on POSIX and keeps /W4 quiet on Windows.
    ::send(fd, payload.data(), static_cast<int>(payload.size()), 0);
}

/// Bound recv() so a negative assertion ("nothing was pushed") cannot hang
/// the test suite: readLineRaw() returns "" on timeout as well as on close.
bool setRecvTimeoutRaw(RawSocket fd, int timeout_ms)
{
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeout_ms);
    return 0 == ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                             reinterpret_cast<const char *>(&value),
                             sizeof(value));
#else
    timeval value{};
    value.tv_sec = timeout_ms / 1000;
    value.tv_usec = (timeout_ms % 1000) * 1000;
    return 0 == ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
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

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);

    // A normal request before authenticating is refused with 9106.
    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":7,"method":"echo","params":{}})");
    const std::string reply = readLineRaw(fd);
    closeRaw(fd);

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

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":3,"method":"auth","params":{"token":"s3cret"}})");
    const std::string authReply = readLineRaw(fd);

    ASSERT_FALSE(authReply.empty());
    const auto parsedAuth = nlohmann::json::parse(authReply);
    EXPECT_EQ(3, parsedAuth["id"].get<int>());
    EXPECT_TRUE(parsedAuth["result"].value("authenticated", false));

    // The same connection now carries normal requests.
    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":4,"method":"echo","params":{"after":true}})");
    const std::string echoReply = readLineRaw(fd);
    closeRaw(fd);

    ASSERT_FALSE(echoReply.empty());
    const auto parsedEcho = nlohmann::json::parse(echoReply);
    EXPECT_EQ(4, parsedEcho["id"].get<int>());
    EXPECT_TRUE(parsedEcho["result"].value("after", false));

    server->stop();
}

// ---------------------------------------------------------------------------
// Market-data push (subscribe_market) — transport level
//
// These cover the wire behaviour an external strategy runtime relies on:
// a subscription reply, server-initiated notifications with no `id`, symbol
// filtering, and parameter validation.
// ---------------------------------------------------------------------------

TEST(ControlPush, SubscribeThenPublishDeliversClosedKline)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":1,"method":"subscribe_market","params":{"channel":"kline","symbols":["BTC_USDT"],"market_type":"futures"}})");
    const auto reply = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ(1, reply["id"].get<int>());
    EXPECT_TRUE(reply["result"]["subscribed"].get<bool>());
    EXPECT_EQ("kline", reply["result"]["channel"].get<std::string>());

    server->publish({ "kline", "BTC_USDT", "futures" }, "market.kline",
                    { { "symbol", "BTC_USDT" },
                      { "market_type", "futures" },
                      { "close", 50000.0 },
                      { "closed", true } });

    const auto note = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ("market.kline", note["method"].get<std::string>());
    EXPECT_FALSE(note.contains("id")); // Notifications carry no id.
    EXPECT_EQ("BTC_USDT", note["params"]["symbol"].get<std::string>());
    EXPECT_DOUBLE_EQ(50000.0, note["params"]["close"].get<double>());
    EXPECT_TRUE(note["params"]["closed"].get<bool>());

    closeRaw(fd);
    server->stop();
}

TEST(ControlPush, SymbolFilterSuppressesOtherSymbols)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":2,"method":"subscribe_market","params":{"channel":"kline","symbols":["BTC_USDT"]}})");
    const auto reply = nlohmann::json::parse(readLineRaw(fd));
    ASSERT_TRUE(reply["result"]["subscribed"].get<bool>());

    // A symbol the session did not subscribe to must not arrive.
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 300));
    server->publish({ "kline", "ETH_USDT", "futures" }, "market.kline",
                    { { "symbol", "ETH_USDT" } });
    EXPECT_TRUE(readLineRaw(fd).empty());

    // The subscribed symbol still does.
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));
    server->publish({ "kline", "BTC_USDT", "futures" }, "market.kline",
                    { { "symbol", "BTC_USDT" } });
    const auto note = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ("BTC_USDT", note["params"]["symbol"].get<std::string>());

    closeRaw(fd);
    server->stop();
}

TEST(ControlPush, UnknownChannelIsRejected)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":3,"method":"subscribe_market","params":{"channel":"ticker"}})");
    const auto reply = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ(-32602, reply["error"]["code"].get<int>());

    closeRaw(fd);
    server->stop();
}

// ---------------------------------------------------------------------------
// Order-event push (subscribe_orders) and multi-channel sessions
// ---------------------------------------------------------------------------

TEST(ControlPush, TwoChannelsOnOneSessionBothDeliver)
{
    // One connection can watch closed klines AND order events: each channel
    // has its own subscription entry, so subscribing the second does not
    // replace the first (the old single-subscription design would have).
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":1,"method":"subscribe_market","params":{"symbols":["BTC_USDT"],"market_type":"futures"}})");
    const auto klineReply = nlohmann::json::parse(readLineRaw(fd));
    ASSERT_TRUE(klineReply["result"]["subscribed"].get<bool>());
    EXPECT_EQ("kline", klineReply["result"]["channel"].get<std::string>());

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":2,"method":"subscribe_orders","params":{"market_type":"futures"}})");
    const auto orderReply = nlohmann::json::parse(readLineRaw(fd));
    ASSERT_TRUE(orderReply["result"]["subscribed"].get<bool>());
    EXPECT_EQ("order", orderReply["result"]["channel"].get<std::string>());
    EXPECT_TRUE(orderReply["result"]["symbols"].empty());

    // Both channels deliver, in publish order (one FIFO queue per session).
    server->publish({ "kline", "BTC_USDT", "futures" }, "market.kline",
                    { { "symbol", "BTC_USDT" }, { "closed", true } });
    server->publish({ "order", "BTC_USDT", "futures" }, "order.update",
                    { { "event", "submitted" }, { "order_id", "42" } });

    const auto klineNote = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ("market.kline", klineNote["method"].get<std::string>());
    const auto orderNote = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ("order.update", orderNote["method"].get<std::string>());
    EXPECT_EQ("42", orderNote["params"]["order_id"].get<std::string>());
    EXPECT_FALSE(orderNote.contains("id")); // Notifications carry no id.

    closeRaw(fd);
    server->stop();
}

TEST(ControlPush, ReplyIsWrittenBeforeNotificationsPublishedDuringDispatch)
{
    // A handler can publish (the open_order path emits `submitted` from inside
    // the order flow). The notification must not overtake the response to the
    // request that caused it: a line-oriented client reading exactly one line
    // per request would take it for the reply and be off by one forever.
    JsonRpcServer *server_ptr = nullptr;
    MethodRegistry registry;
    registry["publish_then_reply"] = [&server_ptr](const nlohmann::json &params)
    {
        server_ptr->publish({ "order", "BTC_USDT", "futures" }, "order.update",
                            { { "event", "submitted" },
                              { "order_id", "77" } });
        return RpcResult{ params };
    };
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, registry);
    server_ptr = server.get();
    ASSERT_TRUE(server->start());

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":1,"method":"subscribe_orders"})");
    ASSERT_TRUE(nlohmann::json::parse(readLineRaw(fd))["result"]["subscribed"].get<bool>());

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":2,"method":"publish_then_reply","params":{"ok":true}})");

    // First line: the response to id 2, never the notification it published.
    const auto first = nlohmann::json::parse(readLineRaw(fd));
    ASSERT_TRUE(first.contains("id"));
    EXPECT_EQ(2, first["id"].get<int>());
    EXPECT_TRUE(first["result"]["ok"].get<bool>());

    // Second line: the notification, still delivered afterwards.
    const auto note = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ("order.update", note["method"].get<std::string>());
    EXPECT_EQ("77", note["params"]["order_id"].get<std::string>());

    closeRaw(fd);
    server->stop();
}

TEST(ControlPush, UnsubscribingOneChannelKeepsTheOther)
{
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":1,"method":"subscribe_market","params":{"symbols":["BTC_USDT"]}})");
    ASSERT_TRUE(nlohmann::json::parse(readLineRaw(fd))["result"]["subscribed"].get<bool>());
    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":2,"method":"subscribe_orders"})");
    ASSERT_TRUE(nlohmann::json::parse(readLineRaw(fd))["result"]["subscribed"].get<bool>());

    // Clear ONLY the kline channel; the reply names the channel it cleared.
    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":3,"method":"unsubscribe_market"})");
    const auto unsubscribe = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ(3, unsubscribe["id"].get<int>());
    EXPECT_FALSE(unsubscribe["result"]["subscribed"].get<bool>());
    EXPECT_EQ("kline", unsubscribe["result"]["channel"].get<std::string>());

    // The kline channel is silent…
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 300));
    server->publish({ "kline", "BTC_USDT", "futures" }, "market.kline",
                    { { "symbol", "BTC_USDT" } });
    EXPECT_TRUE(readLineRaw(fd).empty());

    // …while the order channel still delivers.
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));
    server->publish({ "order", "BTC_USDT", "futures" }, "order.update",
                    { { "event", "completed" }, { "order_id", "43" } });
    const auto orderNote = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ("order.update", orderNote["method"].get<std::string>());
    EXPECT_EQ("43", orderNote["params"]["order_id"].get<std::string>());

    // Clearing the last channel is idempotent as well.
    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":4,"method":"unsubscribe_orders"})");
    const auto last = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_FALSE(last["result"]["subscribed"].get<bool>());
    EXPECT_EQ("order", last["result"]["channel"].get<std::string>());
    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":5,"method":"unsubscribe_orders"})");
    EXPECT_FALSE(nlohmann::json::parse(readLineRaw(fd))["result"]["subscribed"].get<bool>());

    closeRaw(fd);
    server->stop();
}

TEST(ControlPush, EachChannelRejectsTheOtherChannelsName)
{
    // The channel is a property of the method pair: naming the other channel
    // is refused instead of silently subscribing the wrong stream.
    auto server = std::make_unique<JsonRpcServer>("127.0.0.1", 0, makeRegistry());
    ASSERT_TRUE(server->start());

    const RawSocket fd = connectRaw(server->port());
    ASSERT_NE(kInvalidRawSocket, fd);
    ASSERT_TRUE(setRecvTimeoutRaw(fd, 2000));

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":1,"method":"subscribe_market","params":{"channel":"order"}})");
    const auto marketReply = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ(-32602, marketReply["error"]["code"].get<int>());
    EXPECT_NE(std::string::npos,
              marketReply["error"]["message"].get<std::string>().find("kline"));

    sendLineRaw(fd, R"({"jsonrpc":"2.0","id":2,"method":"subscribe_orders","params":{"channel":"kline"}})");
    const auto ordersReply = nlohmann::json::parse(readLineRaw(fd));
    EXPECT_EQ(-32602, ordersReply["error"]["code"].get<int>());
    EXPECT_NE(std::string::npos,
              ordersReply["error"]["message"].get<std::string>().find("order"));

    closeRaw(fd);
    server->stop();
}
