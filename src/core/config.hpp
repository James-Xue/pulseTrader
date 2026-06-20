#pragma once
// config.hpp — Top-level configuration structs for pulseTrader
//
// Each struct maps to one subsystem and can be loaded from TOML/JSON.
// All fields have sensible defaults so the system can start with a minimal config.

#include <cstdint>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace pulse
{

// ---------------------------------------------------------------------------
// StopMode — stop-loss strategy per position
//
//   1. Fixed     — absolute price level derived from entry price +/- fixed_pct
//   2. Trailing  — tracks best observed price with a percentage offset
//   3. TimeBased — forces close after max_hold_seconds elapsed
// ---------------------------------------------------------------------------
enum class StopMode : std::uint8_t
{
    Fixed,     ///< Absolute price level derived from entry price +/- fixed_pct.
    Trailing,  ///< Tracks best observed price with a percentage offset.
    TimeBased, ///< Forces close after max_hold_seconds elapsed.
};

// ---------------------------------------------------------------------------
// StopLossConfig — stop-loss parameters per position
//
// Fields:
//   1. mode              — Stop mode: Fixed, Trailing, or TimeBased
//   2. fixed_pct         — Fixed stop distance as fraction of entry price (e.g. 0.01 = 1%)
//   3. trailing_pct      — Trailing stop offset as fraction of best price (e.g. 0.005 = 0.5%)
//   4. max_hold_seconds  — Maximum hold duration before forced close (time-based stop)
// ---------------------------------------------------------------------------
struct StopLossConfig
{
    StopMode mode = StopMode::Trailing;   ///< Default to trailing stop.
    double fixed_pct = 0.01;              ///< 1% fixed stop distance.
    double trailing_pct = 0.005;          ///< 0.5% trailing offset.
    std::uint32_t max_hold_seconds = 300; ///< 5-minute max hold.
};

// ---------------------------------------------------------------------------
// TakeProfitConfig — partial take-profit ladder parameters
//
// Fields:
//   1. enabled       — Whether take-profit ladder is active
//   2. targets_pct   — Price targets as fractions above entry (Buy) or below (Sell)
//   3. fractions     — Fraction of position to close at each target (must sum to <= 1.0)
// ---------------------------------------------------------------------------
struct TakeProfitConfig
{
    bool enabled = true;
    std::vector<double> targets_pct = { 0.005, 0.01, 0.02 }; ///< 0.5%, 1%, 2% targets.
    std::vector<double> fractions = { 0.33, 0.33, 0.34 };    ///< Close 33%/33%/34% at each.
};

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
    std::string restBaseUrl = "https://api.gateio.ws";
    std::string wsUrl = "wss://api.gateio.ws/ws/v4/";
    std::string futuresWsUrl = "wss://fx-ws.gateio.ws/v4/ws/usdt"; ///< Gate.io futures WS endpoint (USDT-settled).
    std::string proxyUrl;                               ///< HTTP proxy URL (e.g. "http://127.0.0.1:7897"). Falls back to HTTPS_PROXY env var.
    std::uint32_t restTimeoutMs = 10'000;    ///< Per-request timeout in milliseconds.
    std::uint32_t maxRetries = 3;            ///< Retries before giving up on a request.
    std::uint32_t wsReconnectBaseMs = 1'000; ///< Base backoff for WS reconnection (ms).
    std::uint32_t wsReconnectMaxMs = 30'000; ///< Max backoff cap for WS reconnection (ms).
    bool testnet = false;                     ///< True = use Gate.io testnet (virtual funds, futures only).
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
    std::string baseUrl;            ///< Auto-resolved per backend if empty.
    std::uint32_t heartbeatIntervalSec = 300; ///< AI cycle period (default 5 min).
    std::uint32_t requestTimeoutMs = 30'000;  ///< Max wait for a single LLM request.
    std::uint32_t maxRetries = 2;   ///< Retry count for transient LLM failures.
};

// ---------------------------------------------------------------------------
// TwitterConfig — X (Twitter) API v2 social signal ingestion
//
// Fields:
//   1. enabled      — Whether Twitter feed polling is active
//   2. bearerToken  — X API v2 bearer token for recent-search endpoint
//   3. baseUrl      — X API v2 base URL (default: https://api.twitter.com/2)
//   4. keywords     — Search keywords for filtered stream (comma-separated)
//   5. maxTweets    — Maximum tweets to keep in the rolling window
//   6. pollIntervalSec — Seconds between poll requests
// ---------------------------------------------------------------------------
struct TwitterConfig
{
    bool enabled = false;                              ///< Disabled by default.
    std::string bearerToken;                           ///< X API v2 bearer token.
    std::string baseUrl = "https://api.twitter.com/2"; ///< X API v2 base URL.
    std::vector<std::string> keywords;                 ///< Search keywords.
    std::uint32_t maxTweets = 20;                      ///< Rolling window size.
    std::uint32_t pollIntervalSec = 300;               ///< Poll every 5 min.
};

// ---------------------------------------------------------------------------
// NewsConfig — News article ingestion from NewsAPI or CryptoPanic
//
// Fields:
//   1. enabled      — Whether news feed polling is active
//   2. apiKey       — API key for the news provider
//   3. provider     — Provider name ("newsapi" or "cryptopanic")
//   4. baseUrl      — News provider base URL (auto-resolved if empty)
//   5. keywords     — Search keywords for news articles
//   6. maxArticles  — Maximum articles to keep in the rolling window
//   7. pollIntervalSec — Seconds between poll requests
// ---------------------------------------------------------------------------
struct NewsConfig
{
    bool enabled = false;           ///< Disabled by default.
    std::string apiKey;             ///< News provider API key.
    std::string provider = "newsapi"; ///< "newsapi" or "cryptopanic".
    std::string baseUrl;            ///< Auto-resolved per provider if empty.
    std::vector<std::string> keywords; ///< Search keywords.
    std::uint32_t maxArticles = 20; ///< Rolling window size.
    std::uint32_t pollIntervalSec = 300; ///< Poll every 5 min.
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
//   6. maxSymbolNotional   — Maximum notional exposure per individual symbol (USDT)
//   7. stop_loss           — Default stop-loss configuration for new positions
//   8. take_profit         — Default take-profit ladder configuration
// ---------------------------------------------------------------------------
struct RiskConfig
{
    double maxPositionNotional = 1'000.0; ///< Max open notional value (USDT).
    int maxOpenPositions = 5;             ///< Max concurrent open positions.
    double maxDailyDrawdown = 0.02;       ///< Max daily loss as fraction of equity.
    double maxDrawdown = 0.05;            ///< Max total drawdown before halt.
    std::uint32_t maxOrdersPerSec = 5;    ///< Token-bucket capacity for rate limiting.
    double maxSymbolNotional = 500.0;     ///< Max notional per individual symbol (USDT).
    double max_leverage = 10.0;           ///< Maximum leverage allowed per position (futures).
    double max_margin_used = 0.5;         ///< Maximum fraction of equity used as margin (futures).
    StopLossConfig stop_loss;             ///< Default stop-loss configuration.
    TakeProfitConfig take_profit;         ///< Default take-profit configuration.
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
// StrategyInstanceConfig — one strategy's runtime parameters
//
// Fields:
//   1. name              — strategy class name (e.g. "momentum_scalper")
//   2. symbol            — trading pair this strategy operates on
//   3. order_quantity    — base order size in base currency
//   4. min_confidence    — minimum signal confidence to emit (0.0–1.0)
//   5. enabled           — whether this strategy is active
//   6. poll_interval_ms  — how often the strategy thread polls market data
// ---------------------------------------------------------------------------
struct StrategyInstanceConfig
{
    std::string name;                         ///< Strategy class name (e.g. "momentum_scalper").
    std::string symbol;                       ///< Trading pair (e.g. "BTC_USDT").
    double order_quantity = 0.001;            ///< Base order size in base currency.
    double min_confidence = 0.6;              ///< Minimum confidence to emit a signal.
    bool enabled = true;                      ///< Whether this strategy is active.
    std::uint32_t poll_interval_ms = 500;     ///< Poll interval in milliseconds.
    MarketType market_type = MarketType::Spot; ///< Market type: Spot or Futures.
    double leverage = 1.0;                    ///< Leverage multiplier (1.0 = no leverage / spot).
    MarginMode margin_mode = MarginMode::Cross; ///< Margin mode for futures (Cross or Isolated).
};

// ---------------------------------------------------------------------------
// StrategyConfig — strategy engine configuration (Layer 6)
//
// Fields:
//   1. strategies               — list of active strategy instances
//   2. signal_aggregator_threshold — minimum aggregated confidence to act
//   3. signal_cooldown_sec      — seconds between signals for the same symbol
// ---------------------------------------------------------------------------
struct StrategyConfig
{
    std::vector<StrategyInstanceConfig> strategies; ///< Active strategy instances.
    double signal_aggregator_threshold = 0.7;       ///< Minimum aggregated confidence to act.
    std::uint32_t signal_cooldown_sec = 30;         ///< Cooldown between signals per symbol.
};

// ---------------------------------------------------------------------------
// SqliteConfig — Optional SQLite trade recorder (requires -DPULSE_ENABLE_SQLITE=ON)
//
// Fields:
//   1. enabled — Whether SQLite trade recording is active
//   2. dbPath  — Path to the SQLite database file (created if missing)
// ---------------------------------------------------------------------------
struct SqliteConfig
{
    bool enabled = false;                ///< Disabled by default.
    std::string dbPath = "trades.db";    ///< SQLite database file path.
};

// ---------------------------------------------------------------------------
// PulseConfig — Top-level aggregate: one instance drives the entire system
// ---------------------------------------------------------------------------
struct PulseConfig
{
    ExchangeConfig exchange;
    AiConfig ai;
    TwitterConfig twitter;
    NewsConfig news;
    RiskConfig risk;
    StrategyConfig strategy;
    LogConfig log;
    WebUiConfig webui;
    SqliteConfig sqlite;                ///< SQLite trade recorder config.
    std::vector<std::string> symbols; ///< Symbols to trade, e.g. {"BTC_USDT"}.
    MarketType default_market_type = MarketType::Spot; ///< Default market type for strategies without explicit setting.
};

} // namespace pulse
