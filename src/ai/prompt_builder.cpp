// prompt_builder.cpp — System + user prompt assembly implementation (Layer 4 AI Analysis)
//
// Builds two-part prompts for the LLM analysis cycle:
//   1. System prompt — fixed schema enforcement instructions (identical each cycle)
//   2. User prompt   — dynamic content assembled from live market data
//
// The user prompt follows a structured layout:
//   Section A: Market Overview (ticker summary)
//   Section B: Recent Price Action (kline table, up to 10 rows)
//   Section C: Social Signals (tweets, if available)
//   Section D: News Headlines (news, if available)
//   Section E: Current Strategy Parameters
//
// The LLM reads this and returns a JSON object matching the schema enforced
// by the system prompt. The JSON is then parsed by AIClient into AnalysisResult.

#include "pulse/ai/prompt_builder.hpp"

#include "pulse/logging/logger.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>

namespace pulse::ai
{

namespace
{

/// Maximum number of K-line candles to include in the user prompt.
/// 10 candles gives the LLM enough context for trend detection without
/// overwhelming the context window with excessive data.
constexpr std::size_t kMaxKlinesInPrompt = 10;

/// Format a Unix-millisecond timestamp as seconds (truncated) for readability.
/// The LLM does not need sub-second precision; integer seconds are sufficient
/// for identifying candle timing patterns.
[[nodiscard]] std::int64_t ms_to_seconds(std::int64_t ms)
{
    return ms / 1000;
}

/// Format a double with a fixed number of decimal places for the prompt table.
/// Uses 8 decimal places for prices (crypto can have small values) and
/// 4 decimal places for volumes.
[[nodiscard]] std::string format_price(double val)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8) << val;
    return oss.str();
}

[[nodiscard]] std::string format_volume(double val)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4) << val;
    return oss.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// build — public entry point
//
// Assembles the system prompt (static) and user prompt (dynamic) and returns
// them as a pair. The caller passes both to AIClient::analyze().
// ---------------------------------------------------------------------------
std::pair<std::string, std::string> PromptBuilder::build(
        const MarketSnapshot &snapshot,
        const std::string &tweet_text,
        const std::string &news_text,
        const strategy::StrategyParams &params) const
{
    PULSE_LOG_INFO("ai", "Building prompt for {} ({} klines, tweets={}, news={})",
            snapshot.ticker.symbol,
            snapshot.klines.size(),
            tweet_text.empty() ? "none" : "yes",
            news_text.empty() ? "none" : "yes");

    return {system_prompt(), user_prompt(snapshot, tweet_text, news_text, params)};
}

// ---------------------------------------------------------------------------
// system_prompt — fixed JSON schema enforcement instructions
//
// This prompt is identical for every analysis cycle. It tells the LLM:
//   1. Its role (quantitative trading analyst)
//   2. The exact JSON schema it must output
//   3. Rules for param_deltas (signed adjustments, keep them small)
//   4. Output constraint (JSON only, no surrounding text)
// ---------------------------------------------------------------------------
std::string PromptBuilder::system_prompt()
{
    return R"(You are a quantitative trading analyst. Analyze the market data and social signals provided, then output your analysis as a JSON object matching this exact schema:

{
  "sentiment": "bullish" | "bearish" | "neutral",
  "direction_bias": <float -1.0 to 1.0>,
  "volatility": "low" | "medium" | "high",
  "confidence": <float 0.0 to 1.0>,
  "param_deltas": {
    "order_quantity_delta": <float>,
    "min_confidence_delta": <float>,
    "ema_fast_period_delta": <float>,
    "ema_slow_period_delta": <float>,
    "bb_period_delta": <float>,
    "bb_std_dev_delta": <float>,
    "ob_imbalance_threshold_delta": <float>,
    "cooldown_seconds_delta": <float>,
    "stop_loss_pct_delta": <float>,
    "take_profit_pct_delta": <float>
  }
}

Rules:
- All param_deltas are signed adjustments to current parameter values
- Keep deltas small and conservative (the system clamps extreme values)
- Base your analysis on the data provided, not speculation
- Output ONLY the JSON object, no additional text)";
}

// ---------------------------------------------------------------------------
// user_prompt — dynamic content assembled from live market data
//
// Sections:
//   A. Market Overview — ticker summary (symbol, price, bid/ask, change, volume)
//   B. Recent Price Action — kline table (time, OHLCV)
//   C. Social Signals — concatenated tweets (omitted if empty)
//   D. News Headlines — concatenated news (omitted if empty)
//   E. Current Strategy Parameters — all 11 atomic parameter values
// ---------------------------------------------------------------------------
std::string PromptBuilder::user_prompt(
        const MarketSnapshot &snapshot,
        const std::string &tweet_text,
        const std::string &news_text,
        const strategy::StrategyParams &params) const
{
    std::ostringstream out;

    // --- Section A: Market Overview ---
    const auto &t = snapshot.ticker;
    out << "## Market Overview\n\n";
    out << "- Symbol: " << t.symbol << "\n";
    out << "- Current Price: " << format_price(t.last) << "\n";
    out << "- Bid: " << format_price(t.bid) << " | Ask: " << format_price(t.ask) << "\n";
    out << "- 24h Change: " << format_price(t.change_pct) << "%\n";
    out << "- 24h Volume: " << format_volume(t.volume_24h) << "\n\n";

    // --- Section B: Recent Price Action (K-line table) ---
    // Show up to kMaxKlinesInPrompt most recent closed candles in chronological order.
    // If more candles are available, take the last N (most recent).
    const auto &all_klines = snapshot.klines;
    const std::size_t start = (all_klines.size() > kMaxKlinesInPrompt)
                                      ? (all_klines.size() - kMaxKlinesInPrompt)
                                      : 0;

    out << "## Recent Price Action (last " << (all_klines.size() - start) << " candles)\n\n";
    out << "| Time       | Open       | High       | Low        | Close      | Volume     |\n";
    out << "|------------|------------|------------|------------|------------|------------|\n";

    for (std::size_t i = start; i < all_klines.size(); ++i)
    {
        const auto &k = all_klines[i];
        out << "| " << ms_to_seconds(k.open_time)
            << " | " << format_price(k.open)
            << " | " << format_price(k.high)
            << " | " << format_price(k.low)
            << " | " << format_price(k.close)
            << " | " << format_volume(k.volume)
            << " |\n";
    }
    out << "\n";

    // --- Section C: Social Signals (tweets) ---
    // Only include this section if tweet data is available. An empty section
    // wastes context window tokens and may confuse the LLM.
    if (!tweet_text.empty())
    {
        out << "## Recent Social Signals (Twitter/X)\n\n";
        out << tweet_text << "\n\n";
    }

    // --- Section D: News Headlines ---
    // Only include if news data is available.
    if (!news_text.empty())
    {
        out << "## Recent News Headlines\n\n";
        out << news_text << "\n\n";
    }

    // --- Section E: Current Strategy Parameters ---
    // Load each atomic value with acquire ordering to see the latest writes
    // from ParamAdvisor. The LLM uses these as the baseline for its deltas.
    out << "## Current Strategy Parameters\n\n";
    out << "- order_quantity: " << params.order_quantity.load(std::memory_order_acquire) << "\n";
    out << "- min_confidence: " << params.min_confidence.load(std::memory_order_acquire) << "\n";
    out << "- ema_fast_period: " << params.ema_fast_period.load(std::memory_order_acquire) << "\n";
    out << "- ema_slow_period: " << params.ema_slow_period.load(std::memory_order_acquire) << "\n";
    out << "- bb_period: " << params.bb_period.load(std::memory_order_acquire) << "\n";
    out << "- bb_std_dev: " << params.bb_std_dev.load(std::memory_order_acquire) << "\n";
    out << "- ob_imbalance_threshold: " << params.ob_imbalance_threshold.load(std::memory_order_acquire) << "\n";
    out << "- cooldown_seconds: " << params.cooldown_seconds.load(std::memory_order_acquire) << "\n";
    out << "- stop_loss_pct: " << params.stop_loss_pct.load(std::memory_order_acquire) << "\n";
    out << "- take_profit_pct: " << params.take_profit_pct.load(std::memory_order_acquire) << "\n";

    return out.str();
}

} // namespace pulse::ai
