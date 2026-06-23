// test_ai_pipeline.cpp — Unit tests for AiPipeline orchestrator (Layer 4)
//
// Tests the full pipeline cycle using mocked HTTP transport and disabled feeds.
//
// Tests:
//   1. Successful cycle — mock returns valid response, params updated
//   2. LLM failure — returns PulseError, params unchanged
//   3. Schema mismatch — invalid JSON response, params unchanged
//   4. Social feed failure — pipeline continues without social data
//   5. Zero deltas — cycle succeeds but no params changed

#include "ai/ai_pipeline.hpp"

#include <gtest/gtest.h>

using namespace pulse;
using namespace pulse::ai;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// Helper: valid mock transport returning a successful Claude response
// ---------------------------------------------------------------------------
static AIClient::HttpTransport make_success_transport(double qty_delta = 0.0001)
{
    return [qty_delta](const std::string &, const std::string &,
                        const std::vector<std::string> &) -> Result<nlohmann::json>
    {
        nlohmann::json analysis = {
            {"sentiment",      "bullish"},
            {"confidence",     0.8},
            {"direction_bias", 0.5},
            {"volatility",     "medium"},
            {"param_deltas",   {
                {"order_quantity_delta", qty_delta},
            }},
        };

        // Wrap in Claude response format
        return nlohmann::json{
            {"content", {{{"type", "text"},
                           {"text", analysis.dump()}}}},
        };
    };
}

// Helper: mock transport that returns an HTTP error
static AIClient::HttpTransport make_error_transport()
{
    return [](const std::string &, const std::string &,
              const std::vector<std::string> &) -> Result<nlohmann::json>
    {
        return PulseError{ ErrorCode::HttpError, "HTTP 503" };
    };
}

// Helper: mock transport that returns invalid JSON in content
static AIClient::HttpTransport make_invalid_json_transport()
{
    return [](const std::string &, const std::string &,
              const std::vector<std::string> &) -> Result<nlohmann::json>
    {
        return nlohmann::json{
            {"content", {{{"type", "text"}, {"text", "this is not json"}}}},
        };
    };
}

// Helper: create a pipeline with disabled feeds and injected transport
static AiPipeline make_test_pipeline(AIClient::HttpTransport transport)
{
    AiConfig ai_config;
    ai_config.backend = "claude";
    ai_config.model = "claude-sonnet-4-6";
    ai_config.maxRetries = 0; // No retries in tests

    TwitterConfig tw_config;
    tw_config.enabled = false;

    NewsConfig news_config;
    news_config.enabled = false;

    return AiPipeline(ai_config, tw_config, news_config, std::move(transport));
}

// ---------------------------------------------------------------------------
// 1. Successful cycle — params updated
// ---------------------------------------------------------------------------
TEST(AiPipeline, SuccessfulCycle)
{
    auto pipeline = make_test_pipeline(make_success_transport(0.0002));
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    snapshot.ticker.last = 65000.0;
    StrategyParams params;
    std::vector<StrategyParams *> params_vec{ &params };

    double original_qty = params.order_quantity.load();
    auto result = pipeline.run(snapshot, params_vec);

    ASSERT_TRUE(ok(result));
    EXPECT_EQ(value(result).sentiment, Sentiment::Bullish);
    EXPECT_DOUBLE_EQ(value(result).confidence, 0.8);

    // Param should have been updated
    double new_qty = params.order_quantity.load();
    EXPECT_GT(new_qty, original_qty);
}

// ---------------------------------------------------------------------------
// 2. LLM failure — params unchanged
// ---------------------------------------------------------------------------
TEST(AiPipeline, LLMFailurePreservesParams)
{
    auto pipeline = make_test_pipeline(make_error_transport());
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    StrategyParams params;
    std::vector<StrategyParams *> params_vec{ &params };

    double original_qty = params.order_quantity.load();
    auto result = pipeline.run(snapshot, params_vec);

    EXPECT_FALSE(ok(result));
    EXPECT_EQ(error(result).code, ErrorCode::HttpError);

    // Params should be unchanged
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), original_qty);
}

// ---------------------------------------------------------------------------
// 3. Schema mismatch — params unchanged
// ---------------------------------------------------------------------------
TEST(AiPipeline, SchemaMismatchPreservesParams)
{
    auto pipeline = make_test_pipeline(make_invalid_json_transport());
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    StrategyParams params;
    std::vector<StrategyParams *> params_vec{ &params };

    double original_qty = params.order_quantity.load();
    auto result = pipeline.run(snapshot, params_vec);

    EXPECT_FALSE(ok(result));

    // Params should be unchanged
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), original_qty);
}

// ---------------------------------------------------------------------------
// 4. Zero deltas — cycle succeeds, no params changed
// ---------------------------------------------------------------------------
TEST(AiPipeline, ZeroDeltasNoChange)
{
    auto pipeline = make_test_pipeline(make_success_transport(0.0));
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    StrategyParams params;
    std::vector<StrategyParams *> params_vec{ &params };

    auto result = pipeline.run(snapshot, params_vec);
    ASSERT_TRUE(ok(result));

    // order_quantity_delta was 0, so no change
    EXPECT_DOUBLE_EQ(params.order_quantity.load(), 0.001);
}

// ---------------------------------------------------------------------------
// 5. Component accessors
// ---------------------------------------------------------------------------
TEST(AiPipeline, ComponentAccessors)
{
    auto pipeline = make_test_pipeline(make_success_transport());

    // Should be able to access components
    EXPECT_EQ(pipeline.twitterFeed().size(), 0u);
    EXPECT_EQ(pipeline.newsFeed().size(), 0u);
    EXPECT_EQ(pipeline.paramAdvisor().bounds().size(), 10u);
}

// ---------------------------------------------------------------------------
// lastResult() — interface gap bridge for dashboard
// ---------------------------------------------------------------------------

TEST(AiPipeline, LastResultNullptrBeforeAnyRun)
{
    // Before any run() call, lastResult() must return nullptr.
    auto pipeline = make_test_pipeline(make_success_transport());
    const auto result = pipeline.lastResult();
    EXPECT_EQ(nullptr, result);
}

TEST(AiPipeline, LastResultPopulatedAfterSuccessfulRun)
{
    // After a successful run(), lastResult() must return a non-null pointer
    // with the correct analysis data.
    auto pipeline = make_test_pipeline(make_success_transport(0.0005));
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    snapshot.ticker.last = 65000.0;
    StrategyParams params;
    std::vector<StrategyParams *> params_vec{ &params };

    auto run_result = pipeline.run(snapshot, params_vec);
    ASSERT_TRUE(ok(run_result));

    // lastResult() should now be populated.
    const auto last = pipeline.lastResult();
    ASSERT_NE(nullptr, last);
    EXPECT_EQ(last->sentiment, Sentiment::Bullish);
    EXPECT_DOUBLE_EQ(last->confidence, 0.8);
    EXPECT_DOUBLE_EQ(last->param_deltas.order_quantity_delta, 0.0005);
}

TEST(AiPipeline, LastResultRemainsNullAfterFailedRun)
{
    // After a failed run(), lastResult() must remain nullptr.
    auto pipeline = make_test_pipeline(make_error_transport());
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    StrategyParams params;
    std::vector<StrategyParams *> params_vec{ &params };

    auto run_result = pipeline.run(snapshot, params_vec);
    ASSERT_FALSE(ok(run_result));

    // lastResult() should still be nullptr.
    const auto last = pipeline.lastResult();
    EXPECT_EQ(nullptr, last);
}
