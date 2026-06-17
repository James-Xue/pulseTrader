#pragma once
// ai_pipeline.hpp — AI analysis cycle orchestrator (Layer 4 AI Analysis)
//
// Coordinates the full AI analysis cycle:
//   1. Poll social feeds (Twitter + News) for recent signals
//   2. Build system + user prompt from market data + social signals
//   3. Send prompt to LLM backend and receive structured response
//   4. Validate response against JSON schema
//   5. Apply parameter deltas to StrategyParams atomically
//
// Error resilience:
//   - Each step tolerates failure independently
//   - Social feed errors → prompt built without social data
//   - LLM errors → logged, old params preserved
//   - Schema errors → logged, old params preserved
//   - run() never throws — all errors converted to PulseError
//
// Thread safety:
//   - run() is called from the TaskQueue worker thread (single-threaded)
//   - StrategyParams writes are atomic (lock-free for strategy readers)

#include "pulse/ai/ai_client.hpp"
#include "pulse/ai/analysis_result.hpp"
#include "pulse/ai/news_feed.hpp"
#include "pulse/ai/param_advisor.hpp"
#include "pulse/ai/prompt_builder.hpp"
#include "pulse/ai/twitter_feed.hpp"
#include "pulse/core/config.hpp"
#include "pulse/core/error.hpp"
#include "pulse/strategy/strategy_params.hpp"

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// AiPipeline — orchestrates one complete AI analysis cycle
// ---------------------------------------------------------------------------
class AiPipeline
{
  public:
    /// Construct the pipeline with all component configurations.
    ///
    /// Parameters:
    ///   1. ai_config    — LLM backend settings (backend, model, apiKey)
    ///   2. twitter_config — Twitter feed settings (enabled, bearerToken)
    ///   3. news_config  — News feed settings (enabled, apiKey, provider)
    ///   4. transport    — Optional HTTP transport override (for testing)
    AiPipeline(const AiConfig &ai_config,
               const TwitterConfig &twitter_config,
               const NewsConfig &news_config,
               AIClient::HttpTransport transport = nullptr);

    /// Run one full AI analysis cycle.
    ///
    /// Execution flow:
    ///   1. Poll Twitter feed (if enabled) — failure logged, not fatal
    ///   2. Poll news feed (if enabled) — failure logged, not fatal
    ///   3. Build prompt from market data + social signals
    ///   4. Call LLM and parse response
    ///   5. Apply validated parameter deltas
    ///
    /// Parameters:
    ///   1. snapshot — current market data (ticker + recent klines)
    ///   2. params   — strategy parameters to update atomically
    ///
    /// Returns:
    ///   - AnalysisResult on success (params may have been updated)
    ///   - PulseError on failure (params unchanged)
    [[nodiscard]] Result<AnalysisResult> run(const MarketSnapshot &snapshot,
                                             strategy::StrategyParams &params);

    /// Access the Twitter feed component (for testing / inspection).
    [[nodiscard]] TwitterFeed &twitter_feed();

    /// Access the news feed component (for testing / inspection).
    [[nodiscard]] NewsFeed &news_feed();

    /// Access the parameter advisor (for bounds inspection / tuning).
    [[nodiscard]] ParamAdvisor &param_advisor();

  private:
    TwitterFeed twitter_feed_;    ///< Social signal ingestion (X API v2).
    NewsFeed news_feed_;          ///< News article ingestion (NewsAPI/CryptoPanic).
    PromptBuilder prompt_builder_; ///< Prompt assembly.
    AIClient ai_client_;          ///< LLM HTTP client.
    ParamAdvisor param_advisor_;  ///< Delta validation + atomic apply.
};

} // namespace pulse::ai
