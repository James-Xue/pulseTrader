// test_web_server.cpp — Unit tests for WebServer HTTP + WebSocket server (Layer 9 WebUI)
//
// Tests the WebServer at the component level by starting a real server on an
// OS-assigned port (port 0) and sending raw HTTP requests via TCP sockets.
//
// Test fixture:
//   - Constructs all upstream components with empty/mock configs (never started)
//   - Creates WebServer with authToken="test-token", bindAddress="127.0.0.1", port=0
//   - Starts the server in SetUp(), stops in TearDown()
//   - Provides send_http() helper for raw TCP request/response
//
// Tests:
//   1. DefaultNotRunning           — running() returns false before start()
//   2. StartBindsPort              — start() succeeds, running() true, port() > 0
//   3. StopClearsRunning           — after start+stop, running() false
//   4. AuthRejectsMissingToken     — request without Authorization gets 401
//   5. AuthRejectsWrongToken       — request with wrong token gets 401
//   6. AuthAcceptsValidToken       — request with correct token gets 200
//   7. HostValidationRejects       — spoofed Host header gets 403
//   8. HostValidationAcceptsLocal  — localhost:PORT Host header passes
//   9. StatusEndpointReturnsOk     — GET /api/status returns JSON with status=ok
//  10. SnapshotEndpointReturnsJson — GET /api/snapshot returns JSON (possibly error)

#include "pulse/webui/web_server.hpp"

#include "pulse/ai/ai_pipeline.hpp"
#include "pulse/core/config.hpp"
#include "pulse/exchange/gate_rest_client.hpp"
#include "pulse/exchange/gate_ws_client.hpp"
#include "pulse/execution/order_tracker.hpp"
#include "pulse/market/market_feed.hpp"
#include "pulse/risk/drawdown_guard.hpp"
#include "pulse/risk/order_rate_limiter.hpp"
#include "pulse/risk/position_manager.hpp"
#include "pulse/risk/risk_manager.hpp"
#include "pulse/strategy/strategy_manager.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

// POSIX sockets for raw TCP testing.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace pulse;
using namespace pulse::webui;
using namespace pulse::market;
using namespace pulse::strategy;
using namespace pulse::risk;
using namespace pulse::execution;
using namespace pulse::ai;
using namespace pulse::exchange;

// ---------------------------------------------------------------------------
// Helper: send a raw HTTP request and return the response
// ---------------------------------------------------------------------------

// Renamed to avoid collision with pulse::exchange::HttpResponse.
struct RawHttpResponse
{
    int status_code{ 0 };
    std::string headers;
    std::string body;
};

/// Send an HTTP request to the given host:port and parse the response.
///
/// This is intentionally minimal — just enough to verify status codes and
/// basic JSON bodies for the WebServer tests.
///
/// Parameters:
///   - host_override: if non-empty, replaces the default Host header value
static RawHttpResponse send_http(const std::string &host,
                                  std::uint16_t port,
                                  const std::string &method,
                                  const std::string &path,
                                  const std::string &extra_headers = "",
                                  const std::string &host_override = "")
{
    RawHttpResponse resp;

    // 1. Create a TCP socket.
    const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sock)
    {
        return resp;
    }

    // Set a 2-second receive timeout to avoid blocking forever.
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 2. Connect to the server.
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (0 != ::connect(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)))
    {
        ::close(sock);
        return resp;
    }

    // 3. Send the HTTP request.
    const auto host_value = host_override.empty()
        ? (host + ":" + std::to_string(port))
        : host_override;

    std::string request = method + " " + path + " HTTP/1.1\r\n"
                        + "Host: " + host_value + "\r\n";

    if (!extra_headers.empty())
    {
        request += extra_headers;
    }

    request += "Connection: close\r\n\r\n";

    ::send(sock, request.c_str(), request.size(), 0);

    // 4. Read the response.
    std::string response;
    char buf[4096];
    ssize_t n = 0;

    while (0 < (n = ::recv(sock, buf, sizeof(buf), 0)))
    {
        response.append(buf, static_cast<std::size_t>(n));
    }

    ::close(sock);

    // 5. Parse the status code from the first line (e.g. "HTTP/1.1 200 OK").
    const auto first_line_end = response.find("\r\n");
    if (std::string::npos != first_line_end)
    {
        const auto first_line = response.substr(0, first_line_end);
        const auto space1 = first_line.find(' ');
        if (std::string::npos != space1)
        {
            const auto space2 = first_line.find(' ', space1 + 1);
            const auto code_str = first_line.substr(
                space1 + 1,
                (std::string::npos != space2) ? (space2 - space1 - 1) : std::string::npos
            );
            resp.status_code = std::stoi(code_str);
        }
    }

    // 6. Split headers and body.
    const auto header_end = response.find("\r\n\r\n");
    if (std::string::npos != header_end)
    {
        resp.headers = response.substr(0, header_end);
        resp.body = response.substr(header_end + 4);
    }
    else
    {
        resp.headers = response;
    }

    return resp;
}

/// Build an Authorization header for the given token.
static std::string auth_header(const std::string &token)
{
    return "Authorization: Bearer " + token + "\r\n";
}

// ---------------------------------------------------------------------------
// Test fixture — constructs all upstream components and starts a WebServer
// ---------------------------------------------------------------------------

class WebServerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // 1. WS/REST clients with empty config (never started, no network).
        ExchangeConfig exchange_config;
        ws_client_ = std::make_unique<GateWsClient>(exchange_config);
        rest_client_ = std::make_unique<GateRestClient>(exchange_config);

        // 2. MarketFeed (not started).
        market_feed_ = std::make_unique<MarketFeed>(*ws_client_, *rest_client_);

        // 3. StrategyManager (empty).
        strategy_mgr_ = std::make_unique<StrategyManager>();

        // 4. Risk components.
        risk_config_ = RiskConfig{};
        position_mgr_ = std::make_unique<PositionManager>(risk_config_);
        drawdown_guard_ = std::make_unique<DrawdownGuard>(risk_config_);
        rate_limiter_ = std::make_unique<OrderRateLimiter>(risk_config_.maxOrdersPerSec);
        risk_mgr_ = std::make_unique<RiskManager>(
            risk_config_, *position_mgr_, *drawdown_guard_, *rate_limiter_);

        // 5. OrderTracker.
        order_tracker_ = std::make_unique<OrderTracker>(*ws_client_, *rest_client_);

        // 6. AiPipeline with mock transport.
        AiConfig ai_config;
        ai_config.backend = "claude";
        ai_config.model = "claude-sonnet-4-6";
        ai_config.maxRetries = 0;

        TwitterConfig tw_config;
        tw_config.enabled = false;

        NewsConfig news_config;
        news_config.enabled = false;

        auto mock_transport = [](const std::string &, const std::string &,
                                  const std::vector<std::string> &) -> Result<nlohmann::json>
        {
            return PulseError{ ErrorCode::HttpError, "mock transport" };
        };

        ai_pipeline_ = std::make_unique<AiPipeline>(
            ai_config, tw_config, news_config, mock_transport);

        // 7. WebUI config with auth enabled and port 0 (OS-assigned).
        webui_config_.enabled = true;
        webui_config_.bindAddress = "127.0.0.1";
        webui_config_.port = 0;
        webui_config_.authToken = "test-token";
        webui_config_.maxClients = 4;

        // 8. DashboardState.
        state_ = std::make_unique<DashboardState>(
            webui_config_,
            *market_feed_,
            *strategy_mgr_,
            *risk_mgr_,
            *order_tracker_,
            *ai_pipeline_);

        // 9. WebServer under test.
        server_ = std::make_unique<WebServer>(
            webui_config_, *state_, "/tmp/pulse_test_nonexistent_frontend");
    }

    void TearDown() override
    {
        if (server_)
        {
            server_->stop();
        }
        if (state_)
        {
            state_->stop();
        }
    }

    // --- Components (owned by fixture) ---
    std::unique_ptr<GateWsClient> ws_client_;
    std::unique_ptr<GateRestClient> rest_client_;
    std::unique_ptr<MarketFeed> market_feed_;
    std::unique_ptr<StrategyManager> strategy_mgr_;
    RiskConfig risk_config_;
    std::unique_ptr<PositionManager> position_mgr_;
    std::unique_ptr<DrawdownGuard> drawdown_guard_;
    std::unique_ptr<OrderRateLimiter> rate_limiter_;
    std::unique_ptr<RiskManager> risk_mgr_;
    std::unique_ptr<OrderTracker> order_tracker_;
    std::unique_ptr<AiPipeline> ai_pipeline_;
    WebUiConfig webui_config_;
    std::unique_ptr<DashboardState> state_;
    std::unique_ptr<WebServer> server_;
};

// ---------------------------------------------------------------------------
// 1. DefaultNotRunning — running() returns false before start()
// ---------------------------------------------------------------------------

TEST_F(WebServerTest, DefaultNotRunning)
{
    // A freshly constructed WebServer must not be running.
    EXPECT_FALSE(server_->running());
    EXPECT_EQ(0, server_->port());
}

// ---------------------------------------------------------------------------
// 2. StartBindsPort — start() on port 0 succeeds, running() true, port() > 0
// ---------------------------------------------------------------------------

TEST_F(WebServerTest, StartBindsPort)
{
    const bool started = server_->start();
    EXPECT_TRUE(started);
    EXPECT_TRUE(server_->running());

    // Port should be assigned (non-zero).
    // If us_socket_local_port works, it returns the OS-assigned port.
    // If not, it falls back to config_.port which is 0.
    const auto bound_port = server_->port();
    EXPECT_GT(bound_port, 0) << "Expected OS-assigned port > 0";

    server_->stop();
}

// ---------------------------------------------------------------------------
// 3. StopClearsRunning — after start+stop, running() false
// ---------------------------------------------------------------------------

TEST_F(WebServerTest, StopClearsRunning)
{
    const bool started = server_->start();
    ASSERT_TRUE(started);
    ASSERT_TRUE(server_->running());

    server_->stop();

    EXPECT_FALSE(server_->running());
    EXPECT_EQ(0, server_->port());
}

// ---------------------------------------------------------------------------
// Helper fixture that starts the server for HTTP tests
// ---------------------------------------------------------------------------

class WebServerHttpTest : public WebServerTest
{
  protected:
    void SetUp() override
    {
        WebServerTest::SetUp();

        // Start the server and give the event loop a moment to initialize.
        const bool started = server_->start();
        ASSERT_TRUE(started) << "Failed to start WebServer";

        port_ = server_->port();
        ASSERT_GT(port_, 0) << "Expected a valid listen port";

        // Brief pause for the event loop to be ready.
        std::this_thread::sleep_for(std::chrono::milliseconds{ 50 });
    }

    std::uint16_t port_{ 0 };
};

// ---------------------------------------------------------------------------
// 4. AuthRejectsMissingToken — request without Authorization gets 401
// ---------------------------------------------------------------------------

TEST_F(WebServerHttpTest, AuthRejectsMissingToken)
{
    const auto resp = send_http("127.0.0.1", port_, "GET", "/api/status");
    EXPECT_EQ(401, resp.status_code);
}

// ---------------------------------------------------------------------------
// 5. AuthRejectsWrongToken — request with wrong token gets 401
// ---------------------------------------------------------------------------

TEST_F(WebServerHttpTest, AuthRejectsWrongToken)
{
    const auto resp = send_http("127.0.0.1", port_, "GET", "/api/status",
                                auth_header("wrong-token"));
    EXPECT_EQ(401, resp.status_code);
}

// ---------------------------------------------------------------------------
// 6. AuthAcceptsValidToken — request with correct token gets 200
// ---------------------------------------------------------------------------

TEST_F(WebServerHttpTest, AuthAcceptsValidToken)
{
    const auto resp = send_http("127.0.0.1", port_, "GET", "/api/status",
                                auth_header("test-token"));
    EXPECT_EQ(200, resp.status_code);
}

// ---------------------------------------------------------------------------
// 7. HostValidationRejectsRebinding — spoofed Host header gets 403
// ---------------------------------------------------------------------------

TEST_F(WebServerHttpTest, HostValidationRejectsRebinding)
{
    // Use a spoofed host that is not localhost or 127.0.0.1.
    const auto resp = send_http("127.0.0.1", port_, "GET", "/api/status",
                                auth_header("test-token"),
                                "evil.example.com:80");
    EXPECT_EQ(403, resp.status_code);
}

// ---------------------------------------------------------------------------
// 8. HostValidationAcceptsLocalhost — localhost:PORT Host header passes
// ---------------------------------------------------------------------------

TEST_F(WebServerHttpTest, HostValidationAcceptsLocalhost)
{
    // Override Host to "localhost:PORT" which should be accepted.
    const auto port_str = std::to_string(port_);
    const auto resp = send_http("127.0.0.1", port_, "GET", "/api/status",
                                auth_header("test-token"),
                                "localhost:" + port_str);
    EXPECT_EQ(200, resp.status_code);
}

// ---------------------------------------------------------------------------
// 9. StatusEndpointReturnsOk — GET /api/status returns JSON with status=ok
// ---------------------------------------------------------------------------

TEST_F(WebServerHttpTest, StatusEndpointReturnsOk)
{
    const auto resp = send_http("127.0.0.1", port_, "GET", "/api/status",
                                auth_header("test-token"));
    ASSERT_EQ(200, resp.status_code);

    // Parse the JSON response.
    const auto j = nlohmann::json::parse(resp.body, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Response body is not valid JSON: " << resp.body;

    EXPECT_EQ("ok", j.value("status", ""));
    EXPECT_TRUE(j.contains("uptime_ms"));
    EXPECT_EQ("0.1.0", j.value("version", ""));
}

// ---------------------------------------------------------------------------
// 10. SnapshotEndpointReturnsJson — GET /api/snapshot returns JSON
// ---------------------------------------------------------------------------

TEST_F(WebServerHttpTest, SnapshotEndpointReturnsJson)
{
    const auto resp = send_http("127.0.0.1", port_, "GET", "/api/snapshot",
                                auth_header("test-token"));
    ASSERT_EQ(200, resp.status_code);

    // The response should be valid JSON.
    // Since DashboardState is not started, we expect the "no snapshot" error.
    const auto j = nlohmann::json::parse(resp.body, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Response body is not valid JSON: " << resp.body;

    // Without starting DashboardState, the snapshot is null.
    EXPECT_TRUE(j.contains("error"));
}
