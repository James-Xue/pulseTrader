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

#include "market/kline_buffer.hpp"
#include "market/ticker_cache.hpp"
#include "strategy/strategy_params.hpp"

#include <string>
#include <utility>
#include <vector>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// MarketSnapshot — lightweight aggregate of market data for prompt assembly
//
// Bundles the ticker (latest price, bid/ask, volume) with recent K-line candles
// so the prompt builder has a single object to work with. The heartbeat pipeline
// populates this from TickerCache and KlineBuffer before calling build().
// ---------------------------------------------------------------------------
struct MarketSnapshot
{
    market::Ticker ticker;                    ///< Latest ticker for the symbol.
    std::vector<market::Kline> klines;        ///< Last N closed candles (chronological order).
};

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
    ///   1. snapshot   — current market data (ticker + recent klines)
    ///   2. tweet_text — concatenated recent tweets (empty = no social data)
    ///   3. news_text  — concatenated recent news (empty = no news data)
    ///   4. params     — current strategy parameter values
    ///
    /// Returns: {system_prompt, user_prompt} ready for AIClient::analyze().
    [[nodiscard]] std::pair<std::string, std::string> build(
            const MarketSnapshot &snapshot,
            const std::string &tweet_text,
            const std::string &news_text,
            const strategy::StrategyParams &params) const;

  private:
    /// Build the fixed system prompt that enforces the JSON output schema.
    ///
    /// The schema includes all required fields (sentiment, direction_bias,
    /// volatility, confidence, param_deltas) and formatting rules.
    [[nodiscard]] static std::string system_prompt();

    /// Build the dynamic user prompt with current market data.
    ///
    /// Includes:
    ///   1. Current ticker data (symbol, price, bid/ask, 24h change, volume)
    ///   2. Recent K-line candles in tabular format (up to 10)
    ///   3. Recent tweets (if available)
    ///   4. Recent news headlines (if available)
    ///   5. Current strategy parameter values
    [[nodiscard]] std::string user_prompt(
            const MarketSnapshot &snapshot,
            const std::string &tweet_text,
            const std::string &news_text,
            const strategy::StrategyParams &params) const;
};

} // namespace pulse::ai
