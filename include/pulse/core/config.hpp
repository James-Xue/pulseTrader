#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

// ---------------------------------------------------------------------------
// Exchange configuration
// ---------------------------------------------------------------------------

struct ExchangeConfig {
    std::string apiKey;
    std::string apiSecret;
    std::string restBaseUrl      = "https://api.gateio.ws/api/v4";
    std::string wsUrl            = "wss://api.gateio.ws/ws/v4/";
    std::uint32_t restTimeoutMs  = 5'000;   ///< Per-request timeout.
    std::uint32_t maxRetries     = 3;        ///< Retries before giving up.
};

// ---------------------------------------------------------------------------
// AI analysis configuration
// ---------------------------------------------------------------------------

struct AiConfig {
    std::string backend = "claude";   ///< "claude" or "openai"
    std::string model;                ///< e.g. "claude-sonnet-4-6"
    std::string apiKey;
    std::uint32_t heartbeatIntervalSec = 300;  ///< AI cycle period (5 min).
    std::uint32_t requestTimeoutMs     = 30'000;
};

// ---------------------------------------------------------------------------
// Risk management configuration
// ---------------------------------------------------------------------------

struct RiskConfig {
    double       maxPositionNotional = 1'000.0;  ///< Max open notional (USDT).
    int          maxOpenPositions    = 5;
    double       maxDailyDrawdown    = 0.02;      ///< Fraction of equity.
    double       maxDrawdown          = 0.05;
    std::uint32_t maxOrdersPerSec   = 5;         ///< Token-bucket capacity.
};

// ---------------------------------------------------------------------------
// Logging configuration
// ---------------------------------------------------------------------------

struct LogConfig {
    std::string level      = "info";   ///< trace/debug/info/warn/error/critical
    std::string logDir     = "logs";
    bool        toConsole  = true;
    bool        toFile     = true;
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
