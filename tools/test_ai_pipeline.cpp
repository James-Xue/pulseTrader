// test_ai_pipeline.cpp — Smoke test for AI pipeline (Layer 4+5)
//
// Verifies the full AI analysis cycle end-to-end:
//   1. Constructs an AiPipeline with disabled social feeds
//   2. Creates a synthetic market snapshot
//   3. Runs the pipeline (mock or real LLM)
//   4. Prints the AnalysisResult and updated parameters
//
// Usage:
//   ./test_ai_pipeline --mock          # No API key needed
//   ./test_ai_pipeline                 # Requires API key (from env or .env)
//
// Environment variables (for real LLM mode):
//   AI_BACKEND     — "claude" or "openai" (default: "claude")
//   AI_MODEL       — model identifier (e.g. "claude-sonnet-4-6")
//   AI_API_KEY     — API key for the chosen backend

#include "ai/ai_pipeline.hpp"
#include "core/config.hpp"
#include "logging/logger.hpp"
#include "strategy/strategy_params.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace pulse;
using namespace pulse::ai;
using namespace pulse::strategy;

// ---------------------------------------------------------------------------
// Mock HTTP transport — returns a canned Claude response
// ---------------------------------------------------------------------------
static AIClient::HttpTransport make_mock_transport()
{
    return [](const std::string &url, const std::string &body,
              const std::vector<std::string> &) -> Result<nlohmann::json>
    {
        std::cout << "[MOCK] LLM request to: " << url << "\n";
        std::cout << "[MOCK] Request body length: " << body.size() << " bytes\n";

        // Return a canned analysis result
        nlohmann::json analysis = {
            {"sentiment",      "bullish"},
            {"direction_bias", 0.35},
            {"volatility",     "medium"},
            {"confidence",     0.78},
            {"param_deltas",   {
                {"order_quantity_delta",          0.0001},
                {"min_confidence_delta",          -0.02},
                {"ema_fast_period_delta",         1.0},
                {"ema_slow_period_delta",         0.0},
                {"bb_period_delta",               0.0},
                {"bb_std_dev_delta",              0.0},
                {"ob_imbalance_threshold_delta",  0.01},
                {"cooldown_seconds_delta",        -2.0},
                {"stop_loss_pct_delta",           0.0},
                {"take_profit_pct_delta",         0.0},
            }},
        };

        return nlohmann::json{
            {"content", {{{"type", "text"}, {"text", analysis.dump()}}}},
        };
    };
}

// ---------------------------------------------------------------------------
// Helper: get environment variable with default
// ---------------------------------------------------------------------------
static std::string get_env(const char *name, const std::string &default_val)
{
    const char *val = std::getenv(name);
    return val ? std::string(val) : default_val;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    // Parse arguments
    bool use_mock = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string("--mock") == argv[i])
        {
            use_mock = true;
        }
    }

    // Initialize logging
    pulse::logging::Logger::init(pulse::LogConfig{});

    std::cout << "=== pulseTrader AI Pipeline Smoke Test ===\n\n";

    // 1. Configure AI backend
    AiConfig ai_config;
    if (use_mock)
    {
        ai_config.backend = "claude";
        ai_config.model = "mock-model";
        ai_config.maxRetries = 0;
        std::cout << "Mode: MOCK (no real API calls)\n";
    }
    else
    {
        ai_config.backend = get_env("AI_BACKEND", "claude");
        ai_config.model = get_env("AI_MODEL", "claude-sonnet-4-6");
        ai_config.apiKey = get_env("AI_API_KEY", "");
        ai_config.maxRetries = 1;

        if (ai_config.apiKey.empty())
        {
            std::cerr << "ERROR: AI_API_KEY not set. Use --mock for mock mode.\n";
            return 1;
        }
        std::cout << "Mode: LIVE (backend=" << ai_config.backend
                  << ", model=" << ai_config.model << ")\n";
    }

    // 2. Disable social feeds for smoke test
    TwitterConfig tw_config;
    tw_config.enabled = false;

    NewsConfig news_config;
    news_config.enabled = false;

    // 3. Create pipeline
    AIClient::HttpTransport transport = use_mock ? make_mock_transport() : nullptr;
    AiPipeline pipeline(ai_config, tw_config, news_config, std::move(transport));

    // 4. Create synthetic market snapshot
    MarketSnapshot snapshot;
    snapshot.ticker.symbol = "BTC_USDT";
    snapshot.ticker.last = 65000.0;
    snapshot.ticker.bid = 64999.0;
    snapshot.ticker.ask = 65001.0;
    snapshot.ticker.volume_24h = 15000.0;
    snapshot.ticker.change_pct = 2.5;

    // Add some synthetic K-lines
    for (int i = 0; i < 10; ++i)
    {
        market::Kline k;
        k.open = 64000.0 + i * 100.0;
        k.high = k.open + 200.0;
        k.low = k.open - 100.0;
        k.close = k.open + 50.0;
        k.volume = 500.0;
        k.closed = true;
        snapshot.klines.push_back(k);
    }

    std::cout << "\n--- Market Snapshot ---\n";
    std::cout << "Symbol:     " << snapshot.ticker.symbol << "\n";
    std::cout << "Price:      " << snapshot.ticker.last << "\n";
    std::cout << "Bid/Ask:    " << snapshot.ticker.bid << " / " << snapshot.ticker.ask << "\n";
    std::cout << "24h Change: " << snapshot.ticker.change_pct << "%\n";
    std::cout << "K-lines:    " << snapshot.klines.size() << "\n";

    // 5. Print current params
    StrategyParams params;
    std::cout << "\n--- Parameters (Before) ---\n";
    std::cout << "order_quantity:           " << params.order_quantity.load() << "\n";
    std::cout << "min_confidence:           " << params.min_confidence.load() << "\n";
    std::cout << "ema_fast_period:          " << params.ema_fast_period.load() << "\n";
    std::cout << "ema_slow_period:          " << params.ema_slow_period.load() << "\n";
    std::cout << "bb_period:                " << params.bb_period.load() << "\n";
    std::cout << "bb_std_dev:               " << params.bb_std_dev.load() << "\n";
    std::cout << "ob_imbalance_threshold:   " << params.ob_imbalance_threshold.load() << "\n";
    std::cout << "cooldown_seconds:         " << params.cooldown_seconds.load() << "\n";
    std::cout << "stop_loss_pct:            " << params.stop_loss_pct.load() << "\n";
    std::cout << "take_profit_pct:          " << params.take_profit_pct.load() << "\n";

    // 6. Run the pipeline
    std::cout << "\n--- Running AI Pipeline ---\n";
    auto result = pipeline.run(snapshot, params);

    if (ok(result))
    {
        const auto &r = value(result);
        std::cout << "\n--- Analysis Result ---\n";
        std::cout << "Sentiment:      " << to_string(r.sentiment) << "\n";
        std::cout << "Direction Bias: " << r.direction_bias << "\n";
        std::cout << "Volatility:     " << to_string(r.volatility) << "\n";
        std::cout << "Confidence:     " << r.confidence << "\n";

        std::cout << "\n--- Parameters (After) ---\n";
        std::cout << "order_quantity:           " << params.order_quantity.load() << "\n";
        std::cout << "min_confidence:           " << params.min_confidence.load() << "\n";
        std::cout << "ema_fast_period:          " << params.ema_fast_period.load() << "\n";
        std::cout << "ema_slow_period:          " << params.ema_slow_period.load() << "\n";
        std::cout << "cooldown_seconds:         " << params.cooldown_seconds.load() << "\n";
        std::cout << "stop_loss_pct:            " << params.stop_loss_pct.load() << "\n";
        std::cout << "take_profit_pct:          " << params.take_profit_pct.load() << "\n";

        std::cout << "\n✅ AI pipeline smoke test PASSED\n";
        return 0;
    }
    else
    {
        const auto &err = error(result);
        std::cerr << "\n❌ AI pipeline FAILED: [" << static_cast<std::uint32_t>(err.code)
                  << "] " << err.message << "\n";
        return 1;
    }
}
