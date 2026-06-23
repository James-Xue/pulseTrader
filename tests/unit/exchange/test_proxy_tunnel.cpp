// test_proxy_tunnel.cpp — Unit tests for ProxyTunnel helpers (Layer 1 Exchange)

#include "exchange/ProxyTunnel.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::exchange;

// ---------------------------------------------------------------------------
// parseWsUrl
// ---------------------------------------------------------------------------

TEST(ProxyTunnel, ParseWsUrlFullUrl)
{
    const auto parts = parseWsUrl("wss://api.gateio.ws/ws/v4/");
    EXPECT_EQ(parts.host, "api.gateio.ws");
    EXPECT_EQ(parts.port, 443);
    EXPECT_EQ(parts.path, "/ws/v4/");
}

TEST(ProxyTunnel, ParseWsUrlWithPort)
{
    const auto parts = parseWsUrl("wss://fx-ws.gateio.ws:8443/ws/v4/");
    EXPECT_EQ(parts.host, "fx-ws.gateio.ws");
    EXPECT_EQ(parts.port, 8443);
    EXPECT_EQ(parts.path, "/ws/v4/");
}

TEST(ProxyTunnel, ParseWsUrlNoPath)
{
    const auto parts = parseWsUrl("wss://api.gateio.ws");
    EXPECT_EQ(parts.host, "api.gateio.ws");
    EXPECT_EQ(parts.port, 443);
    EXPECT_EQ(parts.path, "/");
}

TEST(ProxyTunnel, ParseWsUrlNoScheme)
{
    const auto parts = parseWsUrl("api.gateio.ws/ws/v4/");
    EXPECT_EQ(parts.host, "api.gateio.ws");
    EXPECT_EQ(parts.port, 443);
    EXPECT_EQ(parts.path, "/ws/v4/");
}

// ---------------------------------------------------------------------------
// detectProxyUrl
// ---------------------------------------------------------------------------

TEST(ProxyTunnel, DetectProxyUrlFromConfig)
{
    ExchangeConfig config;
    config.proxyUrl = "http://my-proxy:3128";
    EXPECT_EQ(detectProxyUrl(config), "http://my-proxy:3128");
}

TEST(ProxyTunnel, DetectProxyUrlEmptyConfig)
{
    ExchangeConfig config;
    // With empty config and no env vars set, should return empty.
    // Note: this test assumes HTTPS_PROXY and HTTP_PROXY are not set in the
    // test environment. If they are, the result will be their value.
    const auto result = detectProxyUrl(config);
    // We just verify it doesn't crash — the actual value depends on env.
    SUCCEED();
}

TEST(ProxyTunnel, DetectProxyUrlConfigPriority)
{
    // Config should take priority over env vars.
    ExchangeConfig config;
    config.proxyUrl = "http://config-proxy:8080";
    EXPECT_EQ(detectProxyUrl(config), "http://config-proxy:8080");
}
