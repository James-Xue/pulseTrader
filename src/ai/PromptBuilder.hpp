#pragma once
// prompt_builder.hpp — System + user prompt assembly (Layer 4 AI Analysis)
//
// Builds the two-part prompt sent to the LLM each analysis cycle:
//   1. System prompt — fixed instructions that enforce the JSON output schema
//   2. User prompt   — dynamic content with current market data and strategy params
//
// Design rationale:
//   - Separating system vs user prompts matches the Claude/OpenAI message API model
//   - The system prompt is static and can be cached across cycles (same schema)
//   - The user prompt changes every cycle (new ticker, new klines, new social data)
//   - MarketSnapshot bundles the minimum data needed for prompt assembly
//
// Thread safety:
//   - PromptBuilder is stateless and can be shared across threads safely
//   - All methods are const or static — no mutable state

#include "ai/PipelineContext.hpp"
#include "strategy/StrategyParams.hpp"

#include <string>
#include <utility>
#include <vector>

namespace pulse::ai
{
// (MarketSnapshot lives in PipelineContext.hpp — shared cycle input.)

// ---------------------------------------------------------------------------
// PromptBuilder — constructs the LLM prompt for one analysis cycle
//
// Usage:
//   PromptBuilder builder;
//   auto [sys, usr] = builder.build(snapshot, tweets, news, params);
//   auto result = ai_client.analyze(sys, usr);
// ---------------------------------------------------------------------------
class PromptBuilder
{
  public:
    /// Build the system + user prompts for an AI analysis cycle.
    ///
    /// Parameters:
    ///   1. ctx        — aggregated cycle input (market + symbol tickers +
    ///                    per-strategy recent performance)
    ///   2. tweet_text — concatenated recent tweets (empty = no social data)
    ///   3. news_text  — concatenated recent news (empty = no news data)
    ///   4. params     — primary strategy's current parameter values
    ///
    /// Returns: {systemPrompt, userPrompt} ready for AIClient::analyze().
    [[nodiscard]] std::pair<std::string, std::string> build(
            const PipelineContext &ctx,
            const std::string &tweet_text,
            const std::string &news_text,
            const strategy::StrategyParams &params) const;

  private:
    /// Build the fixed system prompt that enforces the JSON output schema.
    ///
    /// The schema includes all required fields (sentiment, direction_bias,
    /// volatility, confidence, param_deltas) and formatting rules.
    [[nodiscard]] static std::string systemPrompt();

    /// Build the dynamic user prompt with current market data.
    ///
    /// Includes:
    ///   1. Current ticker data (symbol, price, bid/ask, 24h change, volume)
    ///   2. One-line tickers for every other traded symbol
    ///   3. Recent K-line candles in tabular format (up to 10)
    ///   4. Recent tweets (if available)
    ///   5. Recent news headlines (if available)
    ///   6. Current strategy parameter values
    ///   7. Per-strategy recent performance table (if available)
    [[nodiscard]] std::string userPrompt(
            const PipelineContext &ctx,
            const std::string &tweet_text,
            const std::string &news_text,
            const strategy::StrategyParams &params) const;
};

} // namespace pulse::ai
