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

#include "ai/AiPipeline.hpp"

#include "logging/Logger.hpp"

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// Constructor — initialize all pipeline components
// ---------------------------------------------------------------------------
AiPipeline::AiPipeline(const AiConfig &ai_config,
                       const TwitterConfig &twitter_config,
                       const NewsConfig &news_config,
                       AIClient::HttpTransport transport)
    : m_twitterFeed{ twitter_config }
    , m_newsFeed{ news_config }
    , m_promptBuilder{}
    , m_aiClient{ ai_config, std::move(transport) }
    , m_paramAdvisor{}
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
                                       std::vector<strategy::StrategyParams *> &allParams)
{
    PULSE_LOG_INFO("ai", "AI pipeline cycle started for symbol={} ({} strategy params)",
                   snapshot.ticker.symbol, allParams.size());

    // 1. Poll Twitter feed (failure is non-fatal)
    std::string tweet_text;
    try
    {
        int new_tweets = m_twitterFeed.poll();
        PULSE_LOG_INFO("ai", "Twitter feed: {} new tweets ({} total)",
                       new_tweets, m_twitterFeed.size());
        tweet_text = m_twitterFeed.recentText(5);
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
        int new_articles = m_newsFeed.poll();
        PULSE_LOG_INFO("ai", "News feed: {} new articles ({} total)",
                       new_articles, m_newsFeed.size());
        news_text = m_newsFeed.recentText(5);
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_WARN("ai", "News feed poll failed: {} — continuing without news data",
                       e.what());
    }

    // 3. Build system + user prompt from available data.
    //    Use the first strategy's params for the prompt (current values shown to LLM).
    //    Precondition: allParams is non-empty (guaranteed by HeartbeatScheduler).
    auto [systemPrompt, userPrompt] = m_promptBuilder.build(
        snapshot, tweet_text, news_text, *allParams[0]);

    PULSE_LOG_INFO("ai", "Prompt built: system={} chars, user={} chars",
                   systemPrompt.size(), userPrompt.size());

    // 4. Call LLM backend and parse response
    auto result = m_aiClient.analyze(systemPrompt, userPrompt);

    if (!ok(result))
    {
        // LLM call failed — log and return error (old params preserved)
        const auto &err = error(result);
        PULSE_LOG_WARN("ai", "LLM analysis failed: [{}] {}",
                       static_cast<std::uint32_t>(err.code), err.message);
        return err;
    }

    // 5. Apply validated parameter deltas to ALL strategy params.
    const auto &analysis = value(result);
    std::vector<heartbeat::OnParamUpdate> updates;
    for (auto *params_ptr : allParams)
    {
        auto batch = m_paramAdvisor.apply(analysis, *params_ptr);
        // Collect updates from the first strategy only (all get the same deltas).
        if (updates.empty())
        {
            updates = std::move(batch);
        }
    }

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
                   toString(analysis.sentiment), analysis.confidence);

    // Store the latest result for WebUI/dashboard retrieval.
    {
        auto ptr = std::make_shared<const AnalysisResult>(analysis);
        std::unique_lock write_lock(m_resultMutex);
        m_lastResult = std::move(ptr);
    }

    return analysis;
}

// ---------------------------------------------------------------------------
// Component accessors
// ---------------------------------------------------------------------------
TwitterFeed &AiPipeline::twitterFeed()
{
    return m_twitterFeed;
}

NewsFeed &AiPipeline::newsFeed()
{
    return m_newsFeed;
}

ParamAdvisor &AiPipeline::paramAdvisor()
{
    return m_paramAdvisor;
}

std::shared_ptr<const AnalysisResult> AiPipeline::lastResult() const noexcept
{
    std::shared_lock read_lock(m_resultMutex);
    return m_lastResult;
}

} // namespace pulse::ai
