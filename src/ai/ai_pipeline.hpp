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

#include "ai/ai_client.hpp"
#include "ai/analysis_result.hpp"
#include "ai/news_feed.hpp"
#include "ai/param_advisor.hpp"
#include "ai/prompt_builder.hpp"
#include "ai/twitter_feed.hpp"
#include "core/config.hpp"
#include "core/error.hpp"
#include "strategy/strategy_params.hpp"

#include <memory>
#include <shared_mutex>
#include <vector>

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
    ///   5. Apply validated parameter deltas to ALL strategy params
    ///
    /// Parameters:
    ///   1. snapshot    — current market data (ticker + recent klines)
    ///   2. all_params  — pointers to each strategy's StrategyParams;
    ///                    the first is used for prompt building (read),
    ///                    all are updated by ParamAdvisor (write)
    ///
    /// Returns:
    ///   - AnalysisResult on success (params may have been updated)
    ///   - PulseError on failure (params unchanged)
    [[nodiscard]] Result<AnalysisResult> run(
        const MarketSnapshot &snapshot,
        std::vector<strategy::StrategyParams *> &all_params);

    /// Access the Twitter feed component (for testing / inspection).
    [[nodiscard]] TwitterFeed &twitter_feed();

    /// Access the news feed component (for testing / inspection).
    [[nodiscard]] NewsFeed &news_feed();

    /// Access the parameter advisor (for bounds inspection / tuning).
    [[nodiscard]] ParamAdvisor &param_advisor();

    /// Returns the most recent AnalysisResult, or nullptr if no cycle has completed.
    ///
    /// Thread-safe: uses shared_mutex for read access.
    /// The returned shared_ptr is immutable and safe to read from any thread.
    [[nodiscard]] std::shared_ptr<const AnalysisResult> last_result() const noexcept;

  private:
    TwitterFeed twitter_feed_;    ///< Social signal ingestion (X API v2).
    NewsFeed news_feed_;          ///< News article ingestion (NewsAPI/CryptoPanic).
    PromptBuilder prompt_builder_; ///< Prompt assembly.
    AIClient ai_client_;          ///< LLM HTTP client.
    ParamAdvisor param_advisor_;  ///< Delta validation + atomic apply.

    /// Cached last analysis result for WebUI/dashboard retrieval.
    mutable std::shared_mutex result_mutex_;
    std::shared_ptr<const AnalysisResult> last_result_{ nullptr };
};

} // namespace pulse::ai
