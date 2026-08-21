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

#include "ai/AIClient.hpp"
#include "ai/AnalysisResult.hpp"
#include "ai/NewsFeed.hpp"
#include "ai/ParamAdvisor.hpp"
#include "ai/PipelineContext.hpp"
#include "ai/PromptBuilder.hpp"
#include "ai/TwitterFeed.hpp"
#include "core/config.hpp"
#include "core/ParamChangeLog.hpp"
#include "core/PulseError.hpp"
#include "strategy/StrategyHandle.hpp"
#include "strategy/StrategyParams.hpp"

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
    ///   5. change_log   — Optional param-change audit log (nullable; the
    ///                     pipeline records every applied delta here).
    AiPipeline(const AiConfig &ai_config,
               const TwitterConfig &twitter_config,
               const NewsConfig &news_config,
               AIClient::HttpTransport transport = nullptr,
               core::ParamChangeLog *change_log = nullptr);

    /// Run one full AI analysis cycle.
    ///
    /// Execution flow:
    ///   1. Poll Twitter feed (if enabled) — failure logged, not fatal
    ///   2. Poll news feed (if enabled) — failure logged, not fatal
    ///   3. Build prompt from the pipeline context + social signals
    ///   4. Call LLM and parse response
    ///   5. Apply validated parameter deltas to ALL strategy params
    ///
    /// Parameters:
    ///   1. ctx      — aggregated cycle input (market snapshot + per-strategy
    ///                 performance); primary params come from handles[0]
    ///   2. handles  — strategy identity + params handles; the first is used
    ///                 for prompt building (read), all are updated by
    ///                 ParamAdvisor (write). Empty → error, nothing runs.
    ///
    /// Returns:
    ///   - AnalysisResult on success (params may have been updated)
    ///   - PulseError on failure (params unchanged)
    [[nodiscard]] Result<AnalysisResult> run(
        const PipelineContext &ctx,
        std::vector<strategy::StrategyHandle> &handles);

    /// Access the Twitter feed component (for testing / inspection).
    [[nodiscard]] TwitterFeed &twitterFeed();

    /// Access the news feed component (for testing / inspection).
    [[nodiscard]] NewsFeed &newsFeed();

    /// Access the parameter advisor (for bounds inspection / tuning).
    [[nodiscard]] ParamAdvisor &paramAdvisor();

    /// The audit log this pipeline records to (nullable when not wired).
    [[nodiscard]] core::ParamChangeLog *changeLog() const noexcept;

    /// Returns the most recent AnalysisResult, or nullptr if no cycle has completed.
    ///
    /// Thread-safe: uses shared_mutex for read access.
    /// The returned shared_ptr is immutable and safe to read from any thread.
    [[nodiscard]] std::shared_ptr<const AnalysisResult> lastResult() const noexcept;

  private:
    TwitterFeed m_twitterFeed;    ///< Social signal ingestion (X API v2).
    NewsFeed m_newsFeed;          ///< News article ingestion (NewsAPI/CryptoPanic).
    PromptBuilder m_promptBuilder; ///< Prompt assembly.
    AIClient m_aiClient;          ///< LLM HTTP client.
    ParamAdvisor m_paramAdvisor;  ///< Delta validation + atomic apply.
    core::ParamChangeLog *m_changeLog{ nullptr }; ///< Audit log (nullable).

    /// Cached last analysis result for WebUI/dashboard retrieval.
    mutable std::shared_mutex m_resultMutex;
    std::shared_ptr<const AnalysisResult> m_lastResult{ nullptr };
};

} // namespace pulse::ai
