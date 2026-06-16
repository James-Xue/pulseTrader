#pragma once
// config.hpp — Top-level configuration structs for pulseTrader
//
// Each struct maps to one subsystem and can be loaded from TOML/JSON.
// All fields have sensible defaults so the system can start with a minimal config.

#include <cstdint>
#include <string>
#include <vector>

namespace pulse
{

// ---------------------------------------------------------------------------
// ExchangeConfig — Gate.io REST + WebSocket connection parameters
//
// Fields:
//   1. apiKey / apiSecret  — HMAC-SHA512 credentials from Gate.io API management
//   2. restBaseUrl         — v4 REST endpoint (public + private)
//   3. wsUrl               — v4 WebSocket endpoint for real-time feeds
//   4. restTimeoutMs       — Per-request HTTP timeout in milliseconds
//   5. maxRetries          — Retry count for transient network failures
// ---------------------------------------------------------------------------
struct ExchangeConfig
{
    std::string apiKey;
    std::string apiSecret;
    std::string restBaseUrl = "https://api.gateio.ws/api/v4";
    std::string wsUrl = "wss://api.gateio.ws/ws/v4/";
    std::uint32_t restTimeoutMs = 5'000;     ///< Per-request timeout in milliseconds.
    std::uint32_t maxRetries = 3;            ///< Retries before giving up on a request.
    std::uint32_t wsReconnectBaseMs = 1'000; ///< Base backoff for WS reconnection (ms).
    std::uint32_t wsReconnectMaxMs = 30'000; ///< Max backoff cap for WS reconnection (ms).
};

// ---------------------------------------------------------------------------
// AiConfig — LLM backend for sentiment / signal analysis
//
// Fields:
//   1. backend             — Provider name ("claude" or "openai")
//   2. model               — Model identifier (e.g. "claude-sonnet-4-6")
//   3. apiKey              — API key for the chosen provider
//   4. heartbeatIntervalSec — Seconds between AI inference cycles (default 5 min)
//   5. requestTimeoutMs    — Max wait time for a single LLM request
// ---------------------------------------------------------------------------
struct AiConfig
{
    std::string backend = "claude"; ///< "claude" or "openai"
    std::string model;              ///< e.g. "claude-sonnet-4-6"
    std::string apiKey;
    std::uint32_t heartbeatIntervalSec = 300; ///< AI cycle period (default 5 min).
    std::uint32_t requestTimeoutMs = 30'000;  ///< Max wait for a single LLM request.
};

// ---------------------------------------------------------------------------
// RiskConfig — Position and drawdown limits
//
// Fields:
//   1. maxPositionNotional — Maximum open notional value in USDT
//   2. maxOpenPositions    — Maximum number of concurrent open positions
//   3. maxDailyDrawdown    — Maximum daily loss as a fraction of equity (e.g. 0.02 = 2%)
//   4. maxDrawdown         — Maximum total drawdown before trading halts
//   5. maxOrdersPerSec     — Token-bucket capacity for order rate limiting
// ---------------------------------------------------------------------------
struct RiskConfig
{
    double maxPositionNotional = 1'000.0; ///< Max open notional value (USDT).
    int maxOpenPositions = 5;             ///< Max concurrent open positions.
    double maxDailyDrawdown = 0.02;       ///< Max daily loss as fraction of equity.
    double maxDrawdown = 0.05;            ///< Max total drawdown before halt.
    std::uint32_t maxOrdersPerSec = 5;    ///< Token-bucket capacity for rate limiting.
};

// ---------------------------------------------------------------------------
// LogConfig — Logging subsystem configuration
//
// Fields:
//   1. level     — Minimum log level: trace/debug/info/warn/error/critical/off
//   2. logDir    — Directory for per-module log files (created if missing)
//   3. toConsole — Whether to also write logs to stdout
//   4. toFile    — Whether to write per-module .log files to logDir
// ---------------------------------------------------------------------------
struct LogConfig
{
    std::string level = "info";  ///< trace/debug/info/warn/error/critical/off
    std::string logDir = "logs"; ///< Directory for per-module log files.
    bool toConsole = true;       ///< Write logs to stdout.
    bool toFile = true;          ///< Write per-module .log files.
};

// ---------------------------------------------------------------------------
// WebUiConfig — Optional WebUI server (requires -DPULSE_ENABLE_WEBUI=ON)
//
// Security model:
//   1. Bind to localhost only by default (no external access)
//   2. Require a bearer token for all endpoints
//   3. Validate the Host header to prevent DNS rebinding attacks
// ---------------------------------------------------------------------------
struct WebUiConfig
{
    bool enabled = false;
    std::string bindAddress = "127.0.0.1"; ///< Localhost only by default.
    std::uint16_t port = 8080;             ///< HTTP/WebSocket listen port.
    std::string authToken;                 ///< Bearer token for all endpoints.
    std::uint32_t maxClients = 4;          ///< Max concurrent WebSocket clients.
};

// ---------------------------------------------------------------------------
// PulseConfig — Top-level aggregate: one instance drives the entire system
// ---------------------------------------------------------------------------
struct PulseConfig
{
    ExchangeConfig exchange;
    AiConfig ai;
    RiskConfig risk;
    LogConfig log;
    WebUiConfig webui;
    std::vector<std::string> symbols; ///< Symbols to trade, e.g. {"BTC_USDT"}.
};

} // namespace pulse
