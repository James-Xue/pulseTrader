// test_heartbeat_scheduler.cpp — HeartbeatScheduler tests (Layer 5)
//
// Uses a mock-transport AiPipeline and an injected snapshot provider. The
// scheduler runs on its own threads; tests poll the pipeline's lastResult()
// with a bounded wait instead of sleeping a fixed duration.

#include "ai/AiPipeline.hpp"
#include "heartbeat/HeartbeatScheduler.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pulse;
using namespace pulse::ai;
using namespace pulse::heartbeat;
using namespace pulse::strategy;

namespace
{

// Mock transport returning a successful analysis (never hits the network).
AIClient::HttpTransport make_success_transport()
{
    return [](const std::string &, const std::string &,
              const std::vector<std::string> &) -> Result<nlohmann::json>
    {
        nlohmann::json analysis = {
            { "sentiment", "neutral" },
            { "confidence", 0.5 },
            { "direction_bias", 0.0 },
            { "volatility", "medium" },
            { "param_deltas", nlohmann::json::object() },
        };
        return nlohmann::json{
            { "content", {{{ "type", "text" }, { "text", analysis.dump() }}} },
        };
    };
}

AiPipeline make_pipeline()
{
    AiConfig ai_config;
    ai_config.backend = "claude";
    ai_config.model = "claude-sonnet-4-6";
    ai_config.maxRetries = 0;
    TwitterConfig tw_config;
    tw_config.enabled = false;
    NewsConfig news_config;
    news_config.enabled = false;
    return AiPipeline(ai_config, tw_config, news_config, make_success_transport());
}

/// Wait (bounded) until pred() is true.
bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

} // anonymous namespace

TEST(HeartbeatScheduler, RunsPipelineWithInjectedProvider)
{
    auto pipeline = make_pipeline();
    AiConfig cfg;
    cfg.heartbeatIntervalSec = 3600; // never fires on its own in the test
    std::vector<StrategyParams> params(1);
    std::vector<StrategyHandle> handles{
        { "momentum_scalper_BTC_USDT", "MomentumScalper", "BTC_USDT",
          "futures", &params[0] },
    };
    std::atomic<int> provider_calls{ 0 };

    HeartbeatScheduler scheduler(cfg, pipeline, handles,
        [&provider_calls]() -> PipelineContext
        {
            ++provider_calls;
            PipelineContext ctx;
            ctx.market.ticker.symbol = "BTC_USDT";
            ctx.market.ticker.last = 65000.0;
            return ctx;
        });

    scheduler.start();
    scheduler.triggerNow();

    EXPECT_TRUE(wait_until([&]()
    {
        return pipeline.lastResult() != nullptr;
    }, std::chrono::seconds(5)));

    scheduler.stop();
    EXPECT_GT(provider_calls.load(), 0);
}

TEST(HeartbeatScheduler, DegradesOnEmptyProvider)
{
    auto pipeline = make_pipeline();
    AiConfig cfg;
    cfg.heartbeatIntervalSec = 3600;
    std::vector<StrategyParams> params(1);
    std::vector<StrategyHandle> handles{
        { "momentum_scalper_BTC_USDT", "MomentumScalper", "BTC_USDT",
          "futures", &params[0] },
    };

    // No provider wired → the cycle still runs with a degraded context.
    HeartbeatScheduler scheduler(cfg, pipeline, handles);
    scheduler.start();
    scheduler.triggerNow();

    EXPECT_TRUE(wait_until([&]()
    {
        return pipeline.lastResult() != nullptr;
    }, std::chrono::seconds(5)));

    scheduler.stop();
}

TEST(HeartbeatScheduler, SurvivesProviderThrow)
{
    auto pipeline = make_pipeline();
    AiConfig cfg;
    cfg.heartbeatIntervalSec = 3600;
    std::vector<StrategyParams> params(1);
    std::vector<StrategyHandle> handles{
        { "momentum_scalper_BTC_USDT", "MomentumScalper", "BTC_USDT",
          "futures", &params[0] },
    };

    HeartbeatScheduler scheduler(cfg, pipeline, handles,
        []() -> PipelineContext
        {
            throw std::runtime_error("provider exploded");
        });

    scheduler.start();
    scheduler.triggerNow();

    EXPECT_TRUE(wait_until([&]()
    {
        return pipeline.lastResult() != nullptr;
    }, std::chrono::seconds(5)));

    scheduler.stop();
}
