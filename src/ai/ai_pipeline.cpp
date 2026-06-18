// ai_pipeline.cpp — AiPipeline implementation (Layer 4 AI Analysis)
//
// Cycle execution flow:
//   1. Poll social feeds (tolerant of failure)
//   2. Build prompt from available data
//   3. Call LLM backend
//   4. Parse and validate response
//   5. Apply parameter deltas with safety bounds
//
// Each step is wrapped in error handling — a failure at any step
// is logged and the pipeline either continues with degraded data
// (social feed failure) or aborts gracefully (LLM failure).

#include "ai/ai_pipeline.hpp"

#include "logging/logger.hpp"

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// Constructor — initialize all pipeline components
// ---------------------------------------------------------------------------
AiPipeline::AiPipeline(const AiConfig &ai_config,
                       const TwitterConfig &twitter_config,
                       const NewsConfig &news_config,
                       AIClient::HttpTransport transport)
    : twitter_feed_{ twitter_config }
    , news_feed_{ news_config }
    , prompt_builder_{}
    , ai_client_{ ai_config, std::move(transport) }
    , param_advisor_{}
{
    PULSE_LOG_INFO("ai", "AiPipeline initialized (backend={}, model={})",
                   ai_config.backend, ai_config.model);
}

// ---------------------------------------------------------------------------
// run — execute one full AI analysis cycle
//
// Error handling strategy:
//   - Social feeds: log warning, continue with empty social data
//   - LLM call: return PulseError, old params preserved
//   - JSON parse: return PulseError, old params preserved
//   - Param apply: always succeeds (clamps out-of-range values)
// ---------------------------------------------------------------------------
Result<AnalysisResult> AiPipeline::run(const MarketSnapshot &snapshot,
                                       strategy::StrategyParams &params)
{
    PULSE_LOG_INFO("ai", "AI pipeline cycle started for symbol={}",
                   snapshot.ticker.symbol);

    // 1. Poll Twitter feed (failure is non-fatal)
    std::string tweet_text;
    try
    {
        int new_tweets = twitter_feed_.poll();
        PULSE_LOG_INFO("ai", "Twitter feed: {} new tweets ({} total)",
                       new_tweets, twitter_feed_.size());
        tweet_text = twitter_feed_.recent_text(5);
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_WARN("ai", "Twitter feed poll failed: {} — continuing without social data",
                       e.what());
    }

    // 2. Poll news feed (failure is non-fatal)
    std::string news_text;
    try
    {
        int new_articles = news_feed_.poll();
        PULSE_LOG_INFO("ai", "News feed: {} new articles ({} total)",
                       new_articles, news_feed_.size());
        news_text = news_feed_.recent_text(5);
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_WARN("ai", "News feed poll failed: {} — continuing without news data",
                       e.what());
    }

    // 3. Build system + user prompt from available data
    auto [system_prompt, user_prompt] = prompt_builder_.build(
        snapshot, tweet_text, news_text, params);

    PULSE_LOG_INFO("ai", "Prompt built: system={} chars, user={} chars",
                   system_prompt.size(), user_prompt.size());

    // 4. Call LLM backend and parse response
    auto result = ai_client_.analyze(system_prompt, user_prompt);

    if (!ok(result))
    {
        // LLM call failed — log and return error (old params preserved)
        const auto &err = error(result);
        PULSE_LOG_WARN("ai", "LLM analysis failed: [{}] {}",
                       static_cast<std::uint32_t>(err.code), err.message);
        return err;
    }

    // 5. Apply validated parameter deltas to StrategyParams
    const auto &analysis = value(result);
    auto updates = param_advisor_.apply(analysis, params);

    if (updates.empty())
    {
        PULSE_LOG_INFO("ai", "No parameter updates applied (all deltas were zero)");
    }
    else
    {
        for (const auto &update : updates)
        {
            PULSE_LOG_INFO("ai", "Param updated: {} {:.4f} → {:.4f}",
                           update.param_name, update.old_value, update.new_value);
        }
    }

    PULSE_LOG_INFO("ai", "AI pipeline cycle completed: sentiment={}, confidence={:.2f}",
                   to_string(analysis.sentiment), analysis.confidence);

    // Store the latest result for WebUI/dashboard retrieval.
    {
        auto ptr = std::make_shared<const AnalysisResult>(analysis);
        std::unique_lock write_lock(result_mutex_);
        last_result_ = std::move(ptr);
    }

    return analysis;
}

// ---------------------------------------------------------------------------
// Component accessors
// ---------------------------------------------------------------------------
TwitterFeed &AiPipeline::twitter_feed()
{
    return twitter_feed_;
}

NewsFeed &AiPipeline::news_feed()
{
    return news_feed_;
}

ParamAdvisor &AiPipeline::param_advisor()
{
    return param_advisor_;
}

std::shared_ptr<const AnalysisResult> AiPipeline::last_result() const noexcept
{
    std::shared_lock read_lock(result_mutex_);
    return last_result_;
}

} // namespace pulse::ai
