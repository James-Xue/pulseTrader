#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

// ---------------------------------------------------------------------------
// Exchange configuration
// ---------------------------------------------------------------------------

struct ExchangeConfig {
    std::string api_key;
    std::string api_secret;
    std::string rest_base_url    = "https://api.gateio.ws/api/v4";
    std::string ws_url           = "wss://api.gateio.ws/ws/v4/";
    std::uint32_t rest_timeout_ms = 5'000;   ///< Per-request timeout.
    std::uint32_t max_retries     = 3;        ///< Retries before giving up.
};

// ---------------------------------------------------------------------------
// AI analysis configuration
// ---------------------------------------------------------------------------

struct AiConfig {
    std::string backend = "claude";   ///< "claude" or "openai"
    std::string model;                ///< e.g. "claude-sonnet-4-6"
    std::string api_key;
    std::uint32_t heartbeat_interval_sec = 300;  ///< AI cycle period (5 min).
    std::uint32_t request_timeout_ms     = 30'000;
};

// ---------------------------------------------------------------------------
// Risk management configuration
// ---------------------------------------------------------------------------

struct RiskConfig {
    double       max_position_notional = 1'000.0;  ///< Max open notional (USDT).
    int          max_open_positions    = 5;
    double       max_daily_drawdown    = 0.02;      ///< Fraction of equity.
    double       max_drawdown          = 0.05;
    std::uint32_t max_orders_per_sec   = 5;         ///< Token-bucket capacity.
};

// ---------------------------------------------------------------------------
// Logging configuration
// ---------------------------------------------------------------------------

struct LogConfig {
    std::string level      = "info";   ///< trace/debug/info/warn/error/critical
    std::string log_dir    = "logs";
    bool        to_console = true;
    bool        to_file    = true;
};

// ---------------------------------------------------------------------------
// Top-level configuration
// ---------------------------------------------------------------------------

struct PulseConfig {
    ExchangeConfig       exchange;
    AiConfig             ai;
    RiskConfig           risk;
    LogConfig            log;
    std::vector<std::string> symbols;  ///< Symbols to trade, e.g. {"BTC_USDT"}.
};

} // namespace pulse
