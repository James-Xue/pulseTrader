// test_analysis_result.cpp — Unit tests for AnalysisResult JSON schema (Layer 4)
//
// Tests:
//   1. Default construction — all fields at safe defaults
//   2. to_json — correct serialization of all fields
//   3. from_json — correct deserialization with all fields present
//   4. from_json — missing optional fields default to safe values
//   5. from_json — missing required fields throws
//   6. from_json — invalid types throw
//   7. from_json — invalid sentiment string defaults to Neutral
//   8. Round-trip — to_json → from_json preserves values

#include "ai/analysis_result.hpp"

#include <gtest/gtest.h>

using namespace pulse::ai;

// ---------------------------------------------------------------------------
// 1. Default construction
// ---------------------------------------------------------------------------
TEST(AnalysisResult, DefaultConstruction)
{
    AnalysisResult r;
    EXPECT_EQ(r.sentiment, Sentiment::Neutral);
    EXPECT_DOUBLE_EQ(r.direction_bias, 0.0);
    EXPECT_EQ(r.volatility, VolatilityForecast::Medium);
    EXPECT_DOUBLE_EQ(r.confidence, 0.0);

    // All deltas should be zero
    EXPECT_DOUBLE_EQ(r.param_deltas.order_quantity_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.min_confidence_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.ema_fast_period_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.ema_slow_period_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.bb_period_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.bb_std_dev_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.ob_imbalance_threshold_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.cooldown_seconds_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.stop_loss_pct_delta, 0.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.take_profit_pct_delta, 0.0);
}

// ---------------------------------------------------------------------------
// 2. to_json — serialization
// ---------------------------------------------------------------------------
TEST(AnalysisResult, ToJson)
{
    AnalysisResult r;
    r.sentiment = Sentiment::Bullish;
    r.direction_bias = 0.42;
    r.volatility = VolatilityForecast::High;
    r.confidence = 0.85;
    r.param_deltas.order_quantity_delta = 0.0002;
    r.param_deltas.ema_fast_period_delta = -1.0;

    nlohmann::json j = r;

    EXPECT_EQ(j["sentiment"], "bullish");
    EXPECT_DOUBLE_EQ(j["direction_bias"].get<double>(), 0.42);
    EXPECT_EQ(j["volatility"], "high");
    EXPECT_DOUBLE_EQ(j["confidence"].get<double>(), 0.85);
    EXPECT_DOUBLE_EQ(j["param_deltas"]["order_quantity_delta"].get<double>(), 0.0002);
    EXPECT_DOUBLE_EQ(j["param_deltas"]["ema_fast_period_delta"].get<double>(), -1.0);
}

// ---------------------------------------------------------------------------
// 3. from_json — full deserialization
// ---------------------------------------------------------------------------
TEST(AnalysisResult, FromJsonFull)
{
    nlohmann::json j = {
        {"sentiment",      "bearish"},
        {"direction_bias", -0.3},
        {"volatility",     "low"},
        {"confidence",     0.72},
        {"param_deltas",   {
            {"order_quantity_delta",          -0.0001},
            {"min_confidence_delta",          0.05},
            {"ema_fast_period_delta",         1.0},
            {"ema_slow_period_delta",         -2.0},
            {"bb_period_delta",               0.5},
            {"bb_std_dev_delta",              -0.1},
            {"ob_imbalance_threshold_delta",  0.02},
            {"cooldown_seconds_delta",        -3.0},
            {"stop_loss_pct_delta",           0.001},
            {"take_profit_pct_delta",         -0.0005},
        }},
    };

    AnalysisResult r = j.get<AnalysisResult>();

    EXPECT_EQ(r.sentiment, Sentiment::Bearish);
    EXPECT_DOUBLE_EQ(r.direction_bias, -0.3);
    EXPECT_EQ(r.volatility, VolatilityForecast::Low);
    EXPECT_DOUBLE_EQ(r.confidence, 0.72);
    EXPECT_DOUBLE_EQ(r.param_deltas.order_quantity_delta, -0.0001);
    EXPECT_DOUBLE_EQ(r.param_deltas.min_confidence_delta, 0.05);
    EXPECT_DOUBLE_EQ(r.param_deltas.ema_fast_period_delta, 1.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.ema_slow_period_delta, -2.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.bb_period_delta, 0.5);
    EXPECT_DOUBLE_EQ(r.param_deltas.bb_std_dev_delta, -0.1);
    EXPECT_DOUBLE_EQ(r.param_deltas.ob_imbalance_threshold_delta, 0.02);
    EXPECT_DOUBLE_EQ(r.param_deltas.cooldown_seconds_delta, -3.0);
    EXPECT_DOUBLE_EQ(r.param_deltas.stop_loss_pct_delta, 0.001);
    EXPECT_DOUBLE_EQ(r.param_deltas.take_profit_pct_delta, -0.0005);
}

// ---------------------------------------------------------------------------
// 4. from_json — missing optional fields default safely
// ---------------------------------------------------------------------------
TEST(AnalysisResult, FromJsonMinimal)
{
    // Only required fields — direction_bias, volatility, and param_deltas are optional
    nlohmann::json j = {
        {"sentiment",  "neutral"},
        {"confidence", 0.5},
    };

    AnalysisResult r = j.get<AnalysisResult>();

    EXPECT_EQ(r.sentiment, Sentiment::Neutral);
    EXPECT_DOUBLE_EQ(r.direction_bias, 0.0);
    EXPECT_EQ(r.volatility, VolatilityForecast::Medium);
    EXPECT_DOUBLE_EQ(r.confidence, 0.5);
    EXPECT_DOUBLE_EQ(r.param_deltas.order_quantity_delta, 0.0);
}

// ---------------------------------------------------------------------------
// 5. from_json — missing required field throws
// ---------------------------------------------------------------------------
TEST(AnalysisResult, FromJsonMissingSentiment)
{
    nlohmann::json j = {
        {"confidence", 0.5},
    };
    EXPECT_THROW(j.get<AnalysisResult>(), std::invalid_argument);
}

TEST(AnalysisResult, FromJsonMissingConfidence)
{
    nlohmann::json j = {
        {"sentiment", "bullish"},
    };
    EXPECT_THROW(j.get<AnalysisResult>(), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// 6. from_json — invalid types throw
// ---------------------------------------------------------------------------
TEST(AnalysisResult, FromJsonInvalidSentimentType)
{
    nlohmann::json j = {
        {"sentiment",  123},      // Not a string
        {"confidence", 0.5},
    };
    EXPECT_THROW(j.get<AnalysisResult>(), std::invalid_argument);
}

TEST(AnalysisResult, FromJsonInvalidConfidenceType)
{
    nlohmann::json j = {
        {"sentiment",  "bullish"},
        {"confidence", "high"},   // Not a number
    };
    EXPECT_THROW(j.get<AnalysisResult>(), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// 7. Invalid sentiment/volatility strings default to safe values
// ---------------------------------------------------------------------------
TEST(AnalysisResult, InvalidSentimentDefaultsToNeutral)
{
    EXPECT_EQ(sentiment_from_string("unknown"), Sentiment::Neutral);
    EXPECT_EQ(sentiment_from_string(""), Sentiment::Neutral);
    EXPECT_EQ(sentiment_from_string("BULLISH"), Sentiment::Neutral); // Case sensitive
}

TEST(AnalysisResult, InvalidVolatilityDefaultsToMedium)
{
    EXPECT_EQ(volatility_from_string("extreme"), VolatilityForecast::Medium);
    EXPECT_EQ(volatility_from_string(""), VolatilityForecast::Medium);
}

// ---------------------------------------------------------------------------
// 8. Round-trip — to_json → from_json preserves values
// ---------------------------------------------------------------------------
TEST(AnalysisResult, RoundTrip)
{
    AnalysisResult original;
    original.sentiment = Sentiment::Bullish;
    original.direction_bias = 0.65;
    original.volatility = VolatilityForecast::High;
    original.confidence = 0.91;
    original.param_deltas.order_quantity_delta = 0.0003;
    original.param_deltas.min_confidence_delta = -0.05;
    original.param_deltas.ema_fast_period_delta = 2.0;
    original.param_deltas.bb_std_dev_delta = 0.15;
    original.param_deltas.stop_loss_pct_delta = -0.001;

    // Serialize to JSON and back
    nlohmann::json j = original;
    AnalysisResult restored = j.get<AnalysisResult>();

    EXPECT_EQ(restored.sentiment, original.sentiment);
    EXPECT_DOUBLE_EQ(restored.direction_bias, original.direction_bias);
    EXPECT_EQ(restored.volatility, original.volatility);
    EXPECT_DOUBLE_EQ(restored.confidence, original.confidence);
    EXPECT_DOUBLE_EQ(restored.param_deltas.order_quantity_delta,
                     original.param_deltas.order_quantity_delta);
    EXPECT_DOUBLE_EQ(restored.param_deltas.min_confidence_delta,
                     original.param_deltas.min_confidence_delta);
    EXPECT_DOUBLE_EQ(restored.param_deltas.ema_fast_period_delta,
                     original.param_deltas.ema_fast_period_delta);
    EXPECT_DOUBLE_EQ(restored.param_deltas.bb_std_dev_delta,
                     original.param_deltas.bb_std_dev_delta);
    EXPECT_DOUBLE_EQ(restored.param_deltas.stop_loss_pct_delta,
                     original.param_deltas.stop_loss_pct_delta);
}

// ---------------------------------------------------------------------------
// Sentiment and Volatility string conversion
// ---------------------------------------------------------------------------
TEST(AnalysisResult, SentimentToString)
{
    EXPECT_EQ(to_string(Sentiment::Bullish), "bullish");
    EXPECT_EQ(to_string(Sentiment::Bearish), "bearish");
    EXPECT_EQ(to_string(Sentiment::Neutral), "neutral");
}

TEST(AnalysisResult, VolatilityToString)
{
    EXPECT_EQ(to_string(VolatilityForecast::Low), "low");
    EXPECT_EQ(to_string(VolatilityForecast::Medium), "medium");
    EXPECT_EQ(to_string(VolatilityForecast::High), "high");
}

// ---------------------------------------------------------------------------
// Confidence and direction_bias clamping
// ---------------------------------------------------------------------------
TEST(AnalysisResult, ConfidenceClamped)
{
    nlohmann::json j = {
        {"sentiment",  "neutral"},
        {"confidence", 1.5},  // > 1.0
    };
    AnalysisResult r = j.get<AnalysisResult>();
    EXPECT_DOUBLE_EQ(r.confidence, 1.0);

    j["confidence"] = -0.5; // < 0.0
    r = j.get<AnalysisResult>();
    EXPECT_DOUBLE_EQ(r.confidence, 0.0);
}

TEST(AnalysisResult, DirectionBiasClamped)
{
    nlohmann::json j = {
        {"sentiment",      "bullish"},
        {"confidence",     0.5},
        {"direction_bias", 2.0}, // > 1.0
    };
    AnalysisResult r = j.get<AnalysisResult>();
    EXPECT_DOUBLE_EQ(r.direction_bias, 1.0);

    j["direction_bias"] = -3.0; // < -1.0
    r = j.get<AnalysisResult>();
    EXPECT_DOUBLE_EQ(r.direction_bias, -1.0);
}
