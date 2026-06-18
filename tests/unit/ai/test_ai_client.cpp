// test_ai_client.cpp — Unit tests for AIClient (Layer 4)
//
// Uses injected HttpTransport mock to test request building and response parsing
// without real HTTP calls.
//
// Tests:
//   1. Claude backend builds correct request format
//   2. OpenAI backend builds correct request format
//   3. Claude response parsing extracts content correctly
//   4. OpenAI response parsing extracts content correctly
//   5. Invalid JSON response returns PulseError
//   6. HTTP error returns PulseError
//   7. Custom baseUrl is respected
//   8. Missing content field returns error

#include "ai/ai_client.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::ai;

// ---------------------------------------------------------------------------
// Helper: create a mock transport that returns a fixed JSON response
// ---------------------------------------------------------------------------
static AIClient::HttpTransport make_mock(const nlohmann::json &response)
{
    return [response](const std::string &, const std::string &,
                      const std::vector<std::string> &) -> Result<nlohmann::json>
    {
        return response;
    };
}

// Helper: create a mock that captures the request for inspection
struct CapturedRequest
{
    std::string url;
    std::string body;
    std::vector<std::string> headers;
};

static AIClient::HttpTransport make_capturing_mock(CapturedRequest &captured,
                                                    const nlohmann::json &response)
{
    return [&captured, response](const std::string &url, const std::string &body,
                                  const std::vector<std::string> &headers) -> Result<nlohmann::json>
    {
        captured.url = url;
        captured.body = body;
        captured.headers = headers;
        return response;
    };
}

// ---------------------------------------------------------------------------
// 1. Claude backend builds correct request
// ---------------------------------------------------------------------------
TEST(AIClient, ClaudeRequestFormat)
{
    AiConfig config;
    config.backend = "claude";
    config.model = "claude-sonnet-4-6";
    config.apiKey = "test-key";

    // Build a valid Claude response
    nlohmann::json claude_response = {
        {"content", {{{"type", "text"},
                       {"text", R"({"sentiment":"bullish","confidence":0.8,"param_deltas":{}})"}}}},
    };

    CapturedRequest captured;
    AIClient client(config, make_capturing_mock(captured, claude_response));

    auto result = client.analyze("system prompt", "user prompt");
    EXPECT_TRUE(ok(result));

    // Verify URL points to Claude endpoint
    EXPECT_NE(captured.url.find("/v1/messages"), std::string::npos);

    // Verify request body contains model and system prompt
    auto body = nlohmann::json::parse(captured.body);
    EXPECT_EQ(body["model"], "claude-sonnet-4-6");
    EXPECT_EQ(body["system"], "system prompt");

    // Verify headers include x-api-key
    bool has_api_key = false;
    for (const auto &h : captured.headers)
    {
        if (h.find("x-api-key") != std::string::npos)
        {
            has_api_key = true;
        }
    }
    EXPECT_TRUE(has_api_key);
}

// ---------------------------------------------------------------------------
// 2. OpenAI backend builds correct request
// ---------------------------------------------------------------------------
TEST(AIClient, OpenAIRequestFormat)
{
    AiConfig config;
    config.backend = "openai";
    config.model = "gpt-4o";
    config.apiKey = "test-key";

    nlohmann::json openai_response = {
        {"choices", {{{"message", {
            {"role", "assistant"},
            {"content", R"({"sentiment":"bearish","confidence":0.7,"param_deltas":{}})"}
        }}}}},
    };

    CapturedRequest captured;
    AIClient client(config, make_capturing_mock(captured, openai_response));

    auto result = client.analyze("system prompt", "user prompt");
    EXPECT_TRUE(ok(result));

    // Verify URL points to OpenAI endpoint
    EXPECT_NE(captured.url.find("/v1/chat/completions"), std::string::npos);

    // Verify request body has messages array with system and user roles
    auto body = nlohmann::json::parse(captured.body);
    EXPECT_EQ(body["model"], "gpt-4o");
    EXPECT_GE(body["messages"].size(), 2u);

    // Verify Authorization header
    bool has_auth = false;
    for (const auto &h : captured.headers)
    {
        if (h.find("Authorization") != std::string::npos)
        {
            has_auth = true;
        }
    }
    EXPECT_TRUE(has_auth);
}

// ---------------------------------------------------------------------------
// 3. Claude response parsing
// ---------------------------------------------------------------------------
TEST(AIClient, ClaudeResponseParsing)
{
    AiConfig config;
    config.backend = "claude";
    config.model = "claude-sonnet-4-6";

    nlohmann::json response = {
        {"content", {{{"type", "text"},
                       {"text", R"({
                           "sentiment": "bullish",
                           "direction_bias": 0.5,
                           "volatility": "medium",
                           "confidence": 0.85,
                           "param_deltas": {
                               "order_quantity_delta": 0.0001,
                               "ema_fast_period_delta": 1.0
                           }
                       })"}}}},
    };

    AIClient client(config, make_mock(response));
    auto result = client.analyze("sys", "usr");

    ASSERT_TRUE(ok(result));
    EXPECT_EQ(value(result).sentiment, Sentiment::Bullish);
    EXPECT_DOUBLE_EQ(value(result).direction_bias, 0.5);
    EXPECT_DOUBLE_EQ(value(result).confidence, 0.85);
    EXPECT_DOUBLE_EQ(value(result).param_deltas.order_quantity_delta, 0.0001);
}

// ---------------------------------------------------------------------------
// 4. OpenAI response parsing
// ---------------------------------------------------------------------------
TEST(AIClient, OpenAIResponseParsing)
{
    AiConfig config;
    config.backend = "openai";
    config.model = "gpt-4o";

    nlohmann::json response = {
        {"choices", {{{"message", {
            {"role", "assistant"},
            {"content", R"({
                "sentiment": "neutral",
                "confidence": 0.5,
                "param_deltas": {}
            })"}
        }}}}},
    };

    AIClient client(config, make_mock(response));
    auto result = client.analyze("sys", "usr");

    ASSERT_TRUE(ok(result));
    EXPECT_EQ(value(result).sentiment, Sentiment::Neutral);
    EXPECT_DOUBLE_EQ(value(result).confidence, 0.5);
}

// ---------------------------------------------------------------------------
// 5. Invalid JSON response returns PulseError
// ---------------------------------------------------------------------------
TEST(AIClient, InvalidJsonResponse)
{
    AiConfig config;
    config.backend = "claude";

    nlohmann::json response = {
        {"content", {{{"type", "text"}, {"text", "not valid json"}}}},
    };

    AIClient client(config, make_mock(response));
    auto result = client.analyze("sys", "usr");

    EXPECT_FALSE(ok(result));
}

// ---------------------------------------------------------------------------
// 6. HTTP error returns PulseError
// ---------------------------------------------------------------------------
TEST(AIClient, HttpErrorReturnsPulseError)
{
    AiConfig config;
    config.backend = "claude";
    config.maxRetries = 0; // No retries for this test

    auto failing_transport = [](const std::string &, const std::string &,
                                 const std::vector<std::string> &) -> Result<nlohmann::json>
    {
        return PulseError{ ErrorCode::HttpError, "HTTP 500 Internal Server Error" };
    };

    AIClient client(config, failing_transport);
    auto result = client.analyze("sys", "usr");

    EXPECT_FALSE(ok(result));
    EXPECT_EQ(error(result).code, ErrorCode::HttpError);
}

// ---------------------------------------------------------------------------
// 7. Custom baseUrl is respected
// ---------------------------------------------------------------------------
TEST(AIClient, CustomBaseUrl)
{
    AiConfig config;
    config.backend = "claude";
    config.baseUrl = "https://custom-api.example.com";

    CapturedRequest captured;
    nlohmann::json response = {
        {"content", {{{"type", "text"},
                       {"text", R"({"sentiment":"neutral","confidence":0.5,"param_deltas":{}})"}}}},
    };

    AIClient client(config, make_capturing_mock(captured, response));
    (void)client.analyze("sys", "usr");

    EXPECT_NE(captured.url.find("custom-api.example.com"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 8. Missing content in Claude response
// ---------------------------------------------------------------------------
TEST(AIClient, MissingClaudeContent)
{
    AiConfig config;
    config.backend = "claude";
    config.maxRetries = 0;

    nlohmann::json response = {
        {"content", nlohmann::json::array()}, // Empty content array
    };

    AIClient client(config, make_mock(response));
    auto result = client.analyze("sys", "usr");

    EXPECT_FALSE(ok(result));
}
