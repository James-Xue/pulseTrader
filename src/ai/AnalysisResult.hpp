#pragma once
// analysis_result.hpp — AI analysis output schema (Layer 4 AI Analysis)
//
// Defines the fixed JSON schema that the LLM must produce. The system prompt
// enforces this structure; any deviation is discarded and old params persist.
//
// Schema fields:
//   1. sentiment       — "bullish" | "bearish" | "neutral"
//   2. direction_bias  — float in [-1.0, +1.0] (negative = bearish tilt)
//   3. volatility      — "low" | "medium" | "high"
//   4. confidence      — float in [0.0, 1.0]
//   5. param_deltas    — 10 named deltas mapping 1:1 to StrategyParams fields
//
// Thread safety:
//   - AnalysisResult is an immutable value type (passed by const reference)
//   - ParamAdvisor writes the deltas to StrategyParams atomically

#include "strategy/StrategyParams.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace pulse::ai
{

// ---------------------------------------------------------------------------
// Sentiment — directional market sentiment from the LLM
// ---------------------------------------------------------------------------
enum class Sentiment : std::uint8_t
{
    Bullish,  ///< Bullish market outlook — bias towards long positions.
    Bearish,  ///< Bearish market outlook — bias towards short positions.
    Neutral,  ///< No clear directional bias.
};

/// Convert Sentiment to its JSON string representation.
[[nodiscard]] inline std::string toString(Sentiment s)
{
    switch (s)
    {
    case Sentiment::Bullish:
        return "bullish";
    case Sentiment::Bearish:
        return "bearish";
    case Sentiment::Neutral:
    default:
        return "neutral";
    }
}

/// Parse a Sentiment from its JSON string representation.
/// Unknown strings default to Neutral (fail-safe).
[[nodiscard]] inline Sentiment sentimentFromString(const std::string &s)
{
    if ("bullish" == s)
    {
        return Sentiment::Bullish;
    }
    if ("bearish" == s)
    {
        return Sentiment::Bearish;
    }
    return Sentiment::Neutral;
}

// ---------------------------------------------------------------------------
// VolatilityForecast — expected volatility level from the LLM
// ---------------------------------------------------------------------------
enum class VolatilityForecast : std::uint8_t
{
    Low,     ///< Low volatility expected — tighter stops, smaller positions.
    Medium,  ///< Normal volatility — default parameters apply.
    High,    ///< High volatility expected — wider stops, reduced positions.
};

/// Convert VolatilityForecast to its JSON string representation.
[[nodiscard]] inline std::string toString(VolatilityForecast v)
{
    switch (v)
    {
    case VolatilityForecast::Low:
        return "low";
    case VolatilityForecast::High:
        return "high";
    case VolatilityForecast::Medium:
    default:
        return "medium";
    }
}

/// Parse a VolatilityForecast from its JSON string representation.
/// Unknown strings default to Medium (fail-safe).
[[nodiscard]] inline VolatilityForecast volatilityFromString(const std::string &s)
{
    if ("low" == s)
    {
        return VolatilityForecast::Low;
    }
    if ("high" == s)
    {
        return VolatilityForecast::High;
    }
    return VolatilityForecast::Medium;
}

// ---------------------------------------------------------------------------
// ParamDeltas — recommended parameter adjustments from the AI analysis
//
// Each field is a signed delta that ParamAdvisor will clamp and apply
// atomically to the corresponding StrategyParams field.
// ---------------------------------------------------------------------------
struct ParamDeltas
{
    double order_quantity_delta = 0.0;          ///< Adjustment to order size.
    double min_confidence_delta = 0.0;          ///< Adjustment to confidence threshold.
    double ema_fast_period_delta = 0.0;         ///< Adjustment to fast EMA window.
    double ema_slow_period_delta = 0.0;         ///< Adjustment to slow EMA window.
    double bb_period_delta = 0.0;               ///< Adjustment to Bollinger Band window.
    double bb_std_dev_delta = 0.0;              ///< Adjustment to BB std dev multiplier.
    double ob_imbalance_threshold_delta = 0.0;  ///< Adjustment to order book threshold.
    double cooldown_seconds_delta = 0.0;        ///< Adjustment to signal cooldown.
    double stop_loss_pct_delta = 0.0;           ///< Adjustment to stop-loss distance.
    double take_profit_pct_delta = 0.0;         ///< Adjustment to take-profit target.
};

// ---------------------------------------------------------------------------
// AnalysisResult — complete AI analysis output
//
// Immutable value type produced by AIClient after parsing the LLM response.
// ---------------------------------------------------------------------------
struct AnalysisResult
{
    Sentiment sentiment = Sentiment::Neutral;            ///< Directional sentiment.
    double direction_bias = 0.0;                         ///< -1.0 to +1.0.
    VolatilityForecast volatility = VolatilityForecast::Medium; ///< Volatility forecast.
    double confidence = 0.0;                             ///< AI self-assessed confidence.
    ParamDeltas param_deltas;                            ///< Recommended parameter deltas.
};

// ---------------------------------------------------------------------------
// JSON serialization (nlohmann ADL)
// ---------------------------------------------------------------------------

/// Serialize AnalysisResult to JSON.
inline void to_json(nlohmann::json &j, const AnalysisResult &r)
{
    j = nlohmann::json{
        {"sentiment",          toString(r.sentiment)},
        {"direction_bias",     r.direction_bias},
        {"volatility",         toString(r.volatility)},
        {"confidence",         r.confidence},
        {"param_deltas",       {
            {"order_quantity_delta",          r.param_deltas.order_quantity_delta},
            {"min_confidence_delta",          r.param_deltas.min_confidence_delta},
            {"ema_fast_period_delta",         r.param_deltas.ema_fast_period_delta},
            {"ema_slow_period_delta",         r.param_deltas.ema_slow_period_delta},
            {"bb_period_delta",               r.param_deltas.bb_period_delta},
            {"bb_std_dev_delta",              r.param_deltas.bb_std_dev_delta},
            {"ob_imbalance_threshold_delta",  r.param_deltas.ob_imbalance_threshold_delta},
            {"cooldown_seconds_delta",        r.param_deltas.cooldown_seconds_delta},
            {"stop_loss_pct_delta",           r.param_deltas.stop_loss_pct_delta},
            {"take_profit_pct_delta",         r.param_deltas.take_profit_pct_delta},
        }},
    };
}

/// Deserialize AnalysisResult from JSON.
///
/// Throws std::invalid_argument if required fields are missing or have
/// wrong types. ParamAdvisor catches this and converts to PulseError.
inline void from_json(const nlohmann::json &j, AnalysisResult &r)
{
    // 1. Validate top-level required fields exist
    if (!j.contains("sentiment") || !j.contains("confidence"))
    {
        throw std::invalid_argument("Missing required field: sentiment or confidence");
    }

    // 2. Parse sentiment (string enum)
    if (!j["sentiment"].is_string())
    {
        throw std::invalid_argument("Field 'sentiment' must be a string");
    }
    r.sentiment = sentimentFromString(j["sentiment"].get<std::string>());

    // 3. Parse direction_bias (optional, defaults to 0.0)
    if (j.contains("direction_bias") && j["direction_bias"].is_number())
    {
        r.direction_bias = std::clamp(j["direction_bias"].get<double>(), -1.0, 1.0);
    }

    // 4. Parse volatility (optional, defaults to Medium)
    if (j.contains("volatility") && j["volatility"].is_string())
    {
        r.volatility = volatilityFromString(j["volatility"].get<std::string>());
    }

    // 5. Parse confidence (required, [0.0, 1.0])
    if (!j["confidence"].is_number())
    {
        throw std::invalid_argument("Field 'confidence' must be a number");
    }
    r.confidence = std::clamp(j["confidence"].get<double>(), 0.0, 1.0);

    // 6. Parse param_deltas (optional — all default to 0.0 if missing)
    if (j.contains("param_deltas") && j["param_deltas"].is_object())
    {
        const auto &d = j["param_deltas"];
        auto get_delta = [&d](const char *key) -> double
        {
            if (d.contains(key) && d[key].is_number())
            {
                return d[key].get<double>();
            }
            return 0.0;
        };
        r.param_deltas.order_quantity_delta = get_delta("order_quantity_delta");
        r.param_deltas.min_confidence_delta = get_delta("min_confidence_delta");
        r.param_deltas.ema_fast_period_delta = get_delta("ema_fast_period_delta");
        r.param_deltas.ema_slow_period_delta = get_delta("ema_slow_period_delta");
        r.param_deltas.bb_period_delta = get_delta("bb_period_delta");
        r.param_deltas.bb_std_dev_delta = get_delta("bb_std_dev_delta");
        r.param_deltas.ob_imbalance_threshold_delta = get_delta("ob_imbalance_threshold_delta");
        r.param_deltas.cooldown_seconds_delta = get_delta("cooldown_seconds_delta");
        r.param_deltas.stop_loss_pct_delta = get_delta("stop_loss_pct_delta");
        r.param_deltas.take_profit_pct_delta = get_delta("take_profit_pct_delta");
    }
}

} // namespace pulse::ai
