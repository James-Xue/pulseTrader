// config_loader.cpp — TOML configuration file loader
//
// Four-stage pipeline:
//   1. Check file existence
//   2. Parse TOML syntax via toml11
//   3. Resolve "from_env:VAR_NAME" string values
//   4. Map TOML sections to PulseConfig struct fields
//
// All fields are optional — omitted fields retain config.hpp defaults.
// Unknown keys are silently ignored for forward compatibility.

#include "core/config_loader.hpp"

#include <toml.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pulse
{

namespace
{

// ---------------------------------------------------------------------------
// from_env: prefix constant
// ---------------------------------------------------------------------------
constexpr std::string_view kEnvPrefix = "from_env:";

// ---------------------------------------------------------------------------
// resolveEnvVars — recursively walk TOML tree, replacing "from_env:VAR"
// ---------------------------------------------------------------------------
PulseError resolveEnvVars(toml::value &node, const std::string &path)
{
    if (node.is_string())
    {
        std::string &s = node.as_string();
        if (s.size() > kEnvPrefix.size()
            && 0 == s.compare(0, kEnvPrefix.size(), kEnvPrefix))
        {
            const std::string var_name = s.substr(kEnvPrefix.size());
            const char *env_val = std::getenv(var_name.c_str());

            // Unset or empty env vars resolve to empty string.
            // Runtime validation (validateConfig) will catch missing
            // credentials for enabled sections.
            s = (env_val && env_val[0]) ? env_val : "";
        }
        return {};
    }

    if (node.is_table())
    {
        for (auto &[key, child] : node.as_table())
        {
            std::string child_path = path.empty() ? key : path + "." + key;
            auto err = resolveEnvVars(child, child_path);

            if (ErrorCode::Ok != err.code)
            {
                return err;
            }
        }
        return {};
    }

    if (node.is_array())
    {
        auto &arr = node.as_array();

        for (std::size_t i = 0; i < arr.size(); ++i)
        {
            std::string child_path =
                path + "[" + std::to_string(i) + "]";
            auto err = resolveEnvVars(arr[i], child_path);

            if (ErrorCode::Ok != err.code)
            {
                return err;
            }
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// findDouble — extract a double from TOML, accepting both integer and float
//
// toml11 v4 distinguishes TOML integer (int64_t) from float (double).
// toml::find_or<double>() fails if the TOML value is an integer (e.g. 500
// instead of 500.0). This helper handles both cases.
// ---------------------------------------------------------------------------
double findDouble(const toml::value &tbl, const std::string &key,
                   double fallback)
{
    if (!tbl.contains(key))
    {
        return fallback;
    }

    try
    {
        return toml::find<double>(tbl, key);
    }
    catch (...)
    {
        try
        {
            return static_cast<double>(toml::find<std::int64_t>(tbl, key));
        }
        catch (...)
        {
            return fallback;
        }
    }
}

// ---------------------------------------------------------------------------
// findOptionalDouble — optional numeric field: std::nullopt when absent,
// otherwise the TOML value (int or float) via findDouble.
// ---------------------------------------------------------------------------
std::optional<double> findOptionalDouble(const toml::value &tbl,
                                         const std::string &key)
{
    if (!tbl.contains(key))
    {
        return std::nullopt;
    }
    return findDouble(tbl, key, 0.0);
}

// ---------------------------------------------------------------------------
// parseStopMode — string to StopMode enum
// ---------------------------------------------------------------------------
PulseError parseStopMode(const std::string &str, StopMode &out)
{
    if ("Fixed" == str)
    {
        out = StopMode::Fixed;
        return {};
    }
    if ("Trailing" == str)
    {
        out = StopMode::Trailing;
        return {};
    }
    if ("TimeBased" == str)
    {
        out = StopMode::TimeBased;
        return {};
    }

    return PulseError{
        ErrorCode::ConfigInvalidValue,
        "risk.stop_loss.mode must be \"Fixed\", \"Trailing\", or "
        "\"TimeBased\", got \""
            + str + "\""};
}

// ---------------------------------------------------------------------------
// parseMarketType — string to MarketType enum
// ---------------------------------------------------------------------------
PulseError parseMarketType(const std::string &str, MarketType &out)
{
    if ("spot" == str)
    {
        out = MarketType::Spot;
        return {};
    }
    if ("futures" == str)
    {
        out = MarketType::Futures;
        return {};
    }
    if ("cfd" == str)
    {
        out = MarketType::Cfd;
        return {};
    }

    return PulseError{
        ErrorCode::ConfigInvalidValue,
        "market_type must be \"spot\", \"futures\" or \"cfd\", got \""
            + str + "\""};
}

// ---------------------------------------------------------------------------
// parseMarginMode — string to MarginMode enum
// ---------------------------------------------------------------------------
PulseError parseMarginMode(const std::string &str, MarginMode &out)
{
    if ("cross" == str)
    {
        out = MarginMode::Cross;
        return {};
    }
    if ("isolated" == str)
    {
        out = MarginMode::Isolated;
        return {};
    }

    return PulseError{
        ErrorCode::ConfigInvalidValue,
        "margin_mode must be \"cross\" or \"isolated\", got \"" + str + "\""};
}

// ---------------------------------------------------------------------------
// parseOrderType — string to OrderType enum
// ---------------------------------------------------------------------------
PulseError parseOrderType(const std::string &str, OrderType &out)
{
    if ("market" == str)
    {
        out = OrderType::Market;
        return {};
    }
    if ("post_only" == str)
    {
        out = OrderType::PostOnly;
        return {};
    }
    if ("maker_first" == str)
    {
        out = OrderType::MakerFirst;
        return {};
    }

    return PulseError{
        ErrorCode::ConfigInvalidValue,
        "order_type must be \"market\", \"post_only\" or \"maker_first\", got \""
            + str + "\""};
}

// ---------------------------------------------------------------------------
// Section parsers — each reads one TOML [section] into a config struct
// ---------------------------------------------------------------------------

PulseError parseExchange(const toml::value &root, ExchangeConfig &out)
{
    if (!root.contains("exchange"))
    {
        return {};
    }

    const auto &sec = root.at("exchange");

    // Step 1: Read testnet flag FIRST — it determines URL defaults.
    out.testnet = toml::find_or(sec, "testnet", out.testnet);

    // Step 2: Set URL defaults based on network mode.
    // When testnet=true, all URLs switch to testnet endpoints.
    // Explicit TOML values in Step 3 will override these defaults.
    if (out.testnet)
    {
        out.restBaseUrl = url::kTestnetRest;
        out.wsUrl = url::kTestnetSpotWs;
        out.futuresWsUrl = url::kTestnetFuturesWs;
    }

    // Step 3: Load URL fields — explicit TOML values override the defaults above.
    out.restBaseUrl = toml::find_or(sec, "restBaseUrl", out.restBaseUrl);
    out.wsUrl = toml::find_or(sec, "wsUrl", out.wsUrl);
    out.futuresWsUrl = toml::find_or(sec, "futuresWsUrl", out.futuresWsUrl);

    // Step 4: Load remaining fields.
    out.apiKey = toml::find_or(sec, "apiKey", out.apiKey);
    out.apiSecret = toml::find_or(sec, "apiSecret", out.apiSecret);
    try
    {
        out.proxyUrl = toml::find<std::string>(sec, "proxyUrl");
        out.proxyUrlExplicit = true;  // Explicitly set (even if empty)
    }
    catch (const std::out_of_range&)
    {
        // Key not present — will fall back to env vars in detectProxyUrl()
    }
    out.restTimeoutMs =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "restTimeoutMs",
                          static_cast<int>(out.restTimeoutMs)));
    out.maxRetries =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "maxRetries",
                          static_cast<int>(out.maxRetries)));
    out.wsReconnectBaseMs =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "wsReconnectBaseMs",
                          static_cast<int>(out.wsReconnectBaseMs)));
    out.wsReconnectMaxMs =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "wsReconnectMaxMs",
                          static_cast<int>(out.wsReconnectMaxMs)));

    return {};
}

PulseError parseLog(const toml::value &root, LogConfig &out)
{
    if (!root.contains("log"))
    {
        return {};
    }

    const auto &sec = root.at("log");

    out.level = toml::find_or(sec, "level", out.level);
    out.logDir = toml::find_or(sec, "logDir", out.logDir);
    out.toConsole = toml::find_or(sec, "toConsole", out.toConsole);
    out.toFile = toml::find_or(sec, "toFile", out.toFile);

    return {};
}

PulseError parseSymbols(const toml::value &root,
                         std::vector<std::string> &out)
{
    if (!root.contains("symbols"))
    {
        return {};
    }

    const auto &arr = root.at("symbols");

    if (!arr.is_array())
    {
        return PulseError{ErrorCode::ConfigInvalidValue,
                          "symbols must be an array of strings"};
    }

    out.clear();

    for (const auto &elem : arr.as_array())
    {
        if (!elem.is_string())
        {
            return PulseError{ErrorCode::ConfigInvalidValue,
                              "symbols array elements must be strings"};
        }
        out.push_back(elem.as_string());
    }

    return {};
}

PulseError parseAi(const toml::value &root, AiConfig &out)
{
    if (!root.contains("ai"))
    {
        return {};
    }

    const auto &sec = root.at("ai");

    out.backend = toml::find_or(sec, "backend", out.backend);
    out.model = toml::find_or(sec, "model", out.model);
    out.apiKey = toml::find_or(sec, "apiKey", out.apiKey);
    out.baseUrl = toml::find_or(sec, "baseUrl", out.baseUrl);
    out.heartbeatIntervalSec =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "heartbeatIntervalSec",
                          static_cast<int>(out.heartbeatIntervalSec)));
    out.requestTimeoutMs =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "requestTimeoutMs",
                          static_cast<int>(out.requestTimeoutMs)));
    out.maxRetries =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "maxRetries",
                          static_cast<int>(out.maxRetries)));

    return {};
}

PulseError parseTwitter(const toml::value &root, TwitterConfig &out)
{
    if (!root.contains("twitter"))
    {
        return {};
    }

    const auto &sec = root.at("twitter");

    out.enabled = toml::find_or(sec, "enabled", out.enabled);
    out.bearerToken = toml::find_or(sec, "bearerToken", out.bearerToken);
    out.baseUrl = toml::find_or(sec, "baseUrl", out.baseUrl);
    out.maxTweets =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "maxTweets",
                          static_cast<int>(out.maxTweets)));
    out.pollIntervalSec =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "pollIntervalSec",
                          static_cast<int>(out.pollIntervalSec)));

    if (sec.contains("keywords"))
    {
        const auto &arr = sec.at("keywords");

        if (!arr.is_array())
        {
            return PulseError{ErrorCode::ConfigInvalidValue,
                              "twitter.keywords must be an array of strings"};
        }

        out.keywords.clear();

        for (const auto &elem : arr.as_array())
        {
            if (!elem.is_string())
            {
                return PulseError{
                    ErrorCode::ConfigInvalidValue,
                    "twitter.keywords elements must be strings"};
            }
            out.keywords.push_back(elem.as_string());
        }
    }

    return {};
}

PulseError parseNews(const toml::value &root, NewsConfig &out)
{
    if (!root.contains("news"))
    {
        return {};
    }

    const auto &sec = root.at("news");

    out.enabled = toml::find_or(sec, "enabled", out.enabled);
    out.apiKey = toml::find_or(sec, "apiKey", out.apiKey);
    out.provider = toml::find_or(sec, "provider", out.provider);
    out.baseUrl = toml::find_or(sec, "baseUrl", out.baseUrl);
    out.maxArticles =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "maxArticles",
                          static_cast<int>(out.maxArticles)));
    out.pollIntervalSec =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "pollIntervalSec",
                          static_cast<int>(out.pollIntervalSec)));

    if (sec.contains("keywords"))
    {
        const auto &arr = sec.at("keywords");

        if (!arr.is_array())
        {
            return PulseError{ErrorCode::ConfigInvalidValue,
                              "news.keywords must be an array of strings"};
        }

        out.keywords.clear();

        for (const auto &elem : arr.as_array())
        {
            if (!elem.is_string())
            {
                return PulseError{
                    ErrorCode::ConfigInvalidValue,
                    "news.keywords elements must be strings"};
            }
            out.keywords.push_back(elem.as_string());
        }
    }

    return {};
}

PulseError parseStopLoss(const toml::value &sec, StopLossConfig &out)
{
    if (!sec.contains("stop_loss"))
    {
        return {};
    }

    const auto &sub = sec.at("stop_loss");

    if (sub.contains("mode"))
    {
        std::string mode_str = toml::find<std::string>(sub, "mode");
        auto err = parseStopMode(mode_str, out.mode);

        if (ErrorCode::Ok != err.code)
        {
            return err;
        }
    }

    out.fixed_pct = findDouble(sub, "fixed_pct", out.fixed_pct);
    out.trailing_pct = findDouble(sub, "trailing_pct", out.trailing_pct);
    out.max_hold_seconds =
        static_cast<std::uint32_t>(
            toml::find_or(sub, "max_hold_seconds",
                          static_cast<int>(out.max_hold_seconds)));

    return {};
}

PulseError parseTakeProfit(const toml::value &sec, TakeProfitConfig &out)
{
    if (!sec.contains("take_profit"))
    {
        return {};
    }

    const auto &sub = sec.at("take_profit");

    out.enabled = toml::find_or(sub, "enabled", out.enabled);

    if (sub.contains("targets_pct"))
    {
        const auto &arr = sub.at("targets_pct");

        if (!arr.is_array())
        {
            return PulseError{ErrorCode::ConfigInvalidValue,
                              "risk.take_profit.targets_pct must be an array"};
        }

        out.targets_pct.clear();

        for (const auto &elem : arr.as_array())
        {
            out.targets_pct.push_back(toml::get<double>(elem));
        }
    }

    if (sub.contains("fractions"))
    {
        const auto &arr = sub.at("fractions");

        if (!arr.is_array())
        {
            return PulseError{ErrorCode::ConfigInvalidValue,
                              "risk.take_profit.fractions must be an array"};
        }

        out.fractions.clear();

        for (const auto &elem : arr.as_array())
        {
            out.fractions.push_back(toml::get<double>(elem));
        }
    }

    return {};
}

PulseError parseRisk(const toml::value &root, RiskConfig &out)
{
    if (!root.contains("risk"))
    {
        return {};
    }

    const auto &sec = root.at("risk");

    out.maxPositionNotional =
        findDouble(sec, "maxPositionNotional", out.maxPositionNotional);
    out.maxOpenPositions =
        toml::find_or(sec, "maxOpenPositions", out.maxOpenPositions);
    out.maxDailyDrawdown =
        findDouble(sec, "maxDailyDrawdown", out.maxDailyDrawdown);
    out.maxDrawdown =
        findDouble(sec, "maxDrawdown", out.maxDrawdown);
    out.maxOrdersPerSec =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "maxOrdersPerSec",
                          static_cast<int>(out.maxOrdersPerSec)));
    out.maxSymbolNotional =
        findDouble(sec, "maxSymbolNotional", out.maxSymbolNotional);
    out.maxPositionNotionalFutures =
        findOptionalDouble(sec, "maxPositionNotionalFutures");
    out.maxPositionNotionalCfd =
        findOptionalDouble(sec, "maxPositionNotionalCfd");
    out.maxPositionNotionalSpot =
        findOptionalDouble(sec, "maxPositionNotionalSpot");
    out.minAvailableAfterStopUsd =
        findOptionalDouble(sec, "minAvailableAfterStopUsd");
    out.max_leverage =
        findDouble(sec, "max_leverage", out.max_leverage);
    out.max_margin_used =
        findDouble(sec, "max_margin_used", out.max_margin_used);

    // Nested sub-tables.
    auto err = parseStopLoss(sec, out.stop_loss);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseTakeProfit(sec, out.take_profit);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    return {};
}

PulseError parseStrategyInstance(const toml::value &tbl,
                                   StrategyInstanceConfig &out)
{
    out.name = toml::find_or(tbl, "name", out.name);
    out.symbol = toml::find_or(tbl, "symbol", out.symbol);
    out.order_quantity =
        findDouble(tbl, "order_quantity", out.order_quantity);
    out.min_confidence =
        findDouble(tbl, "min_confidence", out.min_confidence);
    out.enabled = toml::find_or(tbl, "enabled", out.enabled);
    out.poll_interval_ms =
        static_cast<std::uint32_t>(
            toml::find_or(tbl, "poll_interval_ms",
                          static_cast<int>(out.poll_interval_ms)));

    // Futures-specific fields.
    out.leverage = findDouble(tbl, "leverage", out.leverage);

    if (tbl.contains("market_type"))
    {
        std::string mt_str = toml::find<std::string>(tbl, "market_type");
        auto err = parseMarketType(mt_str, out.market_type);

        if (ErrorCode::Ok != err.code)
        {
            return err;
        }
    }

    if (tbl.contains("margin_mode"))
    {
        std::string mm_str = toml::find<std::string>(tbl, "margin_mode");
        auto err = parseMarginMode(mm_str, out.margin_mode);

        if (ErrorCode::Ok != err.code)
        {
            return err;
        }
    }

    if (tbl.contains("order_type"))
    {
        std::string ot_str = toml::find<std::string>(tbl, "order_type");
        auto err = parseOrderType(ot_str, out.order_type);

        if (ErrorCode::Ok != err.code)
        {
            return err;
        }
    }

    out.maker_timeout_ms =
        static_cast<std::uint32_t>(
            toml::find_or(tbl, "maker_timeout_ms",
                          static_cast<int>(out.maker_timeout_ms)));

    // Coin-specific parameter channel: `custom_params = { key = value }`
    // inline table (the only sub-table form legal inside an array-of-tables
    // element). Parsed strictly — unlike the top-level "unknown keys are
    // silently ignored" policy, a malformed custom_params is a hard error so
    // a typo'd key type can't silently change strategy behavior.
    if (tbl.contains("custom_params"))
    {
        const toml::value &cp = tbl.at("custom_params"); // Real element ref (no temporary).
        if (!cp.is_table())
        {
            return PulseError{ErrorCode::ConfigInvalidValue,
                              "strategy instance \"custom_params\" must be an "
                              "inline table (e.g. custom_params = { key = 0.05 })"};
        }

        const auto &cp_table = cp.as_table(); // Keep a named reference (GCC -Wdangling-reference).
        for (const auto &[key, value] : cp_table)
        {
            double parsed = 0.0;
            if (value.is_floating())
            {
                parsed = value.as_floating();
            }
            else if (value.is_integer())
            {
                parsed = static_cast<double>(value.as_integer());
            }
            else
            {
                return PulseError{ErrorCode::ConfigInvalidValue,
                                  "strategy instance custom_params key \"" + key
                                      + "\" must be a number (int or float)"};
            }
            out.custom_params[key] = parsed;
        }
    }

    return {};
}

PulseError parseStrategy(const toml::value &root, StrategyConfig &out)
{
    if (!root.contains("strategy"))
    {
        return {};
    }

    const auto &sec = root.at("strategy");

    out.signal_aggregator_threshold = findDouble(
        sec, "signal_aggregator_threshold",
        out.signal_aggregator_threshold);
    out.signal_cooldown_sec =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "signal_cooldown_sec",
                          static_cast<int>(out.signal_cooldown_sec)));
    out.signal_only =
        toml::find_or(sec, "signal_only", out.signal_only);

    if (sec.contains("instances"))
    {
        const auto &arr = sec.at("instances");

        if (!arr.is_array())
        {
            return PulseError{
                ErrorCode::ConfigInvalidValue,
                "strategy.instances must be an array of tables"};
        }

        out.strategies.clear();

        for (std::size_t i = 0; i < arr.as_array().size(); ++i)
        {
            const auto &elem = arr.as_array()[i];

            if (!elem.is_table())
            {
                return PulseError{
                    ErrorCode::ConfigInvalidValue,
                    "strategy.instances[" + std::to_string(i)
                        + "] must be a table"};
            }

            StrategyInstanceConfig inst;
            auto err = parseStrategyInstance(elem, inst);

            if (ErrorCode::Ok != err.code)
            {
                return err;
            }

            out.strategies.push_back(std::move(inst));
        }
    }

    return {};
}

PulseError parseControl(const toml::value &root, ControlConfig &out)
{
    if (!root.contains("control"))
    {
        return {};
    }

    const auto &sec = root.at("control");

    out.enabled = toml::find_or(sec, "enabled", out.enabled);
    out.bindAddress = toml::find_or(sec, "bindAddress", out.bindAddress);
    out.port = static_cast<std::uint16_t>(
        toml::find_or(sec, "port", static_cast<int>(out.port)));
    out.displayTimezone = toml::find_or(
        sec, "display_timezone", out.displayTimezone);

    return {};
}

PulseError parseSqlite(const toml::value &root, SqliteConfig &out)
{
    if (!root.contains("sqlite"))
    {
        return {};
    }

    const auto &sec = root.at("sqlite");

    out.enabled = toml::find_or(sec, "enabled", out.enabled);
    out.dbPath = toml::find_or(sec, "dbPath", out.dbPath);
    out.recordMarketData = toml::find_or(sec, "record_market", out.recordMarketData);

    return {};
}

PulseError parseGrid(const toml::value &root, GridConfig &out)
{
    if (!root.contains("grid"))
    {
        return {};
    }

    const auto &sec = root.at("grid");

    out.enabled = toml::find_or(sec, "enabled", out.enabled);
    out.symbol = toml::find_or(sec, "symbol", out.symbol);
    out.market_type = toml::find_or(sec, "market_type", out.market_type);
    out.levels = toml::find_or(sec, "levels", out.levels);
    out.qty_per_level = toml::find_or(sec, "qty_per_level", out.qty_per_level);
    out.step_mode = toml::find_or(sec, "step_mode", out.step_mode);
    out.step_fixed = toml::find_or(sec, "step_fixed", out.step_fixed);
    out.step_atr_mult = toml::find_or(sec, "step_atr_mult", out.step_atr_mult);
    out.step_min = toml::find_or(sec, "step_min", out.step_min);
    out.step_max = toml::find_or(sec, "step_max", out.step_max);
    out.atr_period = toml::find_or(sec, "atr_period", out.atr_period);
    out.tp_distance_steps = toml::find_or(
        sec, "tp_distance_steps", out.tp_distance_steps);
    out.anchor_offset_steps = toml::find_or(
        sec, "anchor_offset_steps", out.anchor_offset_steps);
    out.lower_reanchor_steps = toml::find_or(
        sec, "lower_reanchor_steps", out.lower_reanchor_steps);
    out.upper_reanchor_steps = toml::find_or(
        sec, "upper_reanchor_steps", out.upper_reanchor_steps);
    out.protect_line_a_steps = toml::find_or(
        sec, "protect_line_a_steps", out.protect_line_a_steps);
    out.protect_line_b_usd = toml::find_or(
        sec, "protect_line_b_usd", out.protect_line_b_usd);
    out.daily_loss_limit_usd = toml::find_or(
        sec, "daily_loss_limit_usd", out.daily_loss_limit_usd);
    out.daily_reset_hour = toml::find_or(
        sec, "daily_reset_hour", out.daily_reset_hour);
    out.reanchor_cooldown_min = toml::find_or(
        sec, "reanchor_cooldown_min", out.reanchor_cooldown_min);
    out.spike_freeze_min = toml::find_or(
        sec, "spike_freeze_min", out.spike_freeze_min);
    out.spike_pct = toml::find_or(sec, "spike_pct", out.spike_pct);
    out.spike_atr_mult = toml::find_or(
        sec, "spike_atr_mult", out.spike_atr_mult);
    out.state_file = toml::find_or(sec, "state_file", out.state_file);
    out.force = toml::find_or(sec, "force", out.force);

    return {};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<PulseConfig> loadConfigFile(const std::filesystem::path &path)
{
    // Stage 1: Check file existence.
    if (!std::filesystem::exists(path))
    {
        return PulseError{ErrorCode::ConfigFileNotFound,
                          "config file not found: " + path.string()};
    }

    // Stage 2: Parse TOML syntax.
    toml::value root;

    try
    {
        root = toml::parse(path.string());
    }
    catch (const toml::syntax_error &e)
    {
        return PulseError{ErrorCode::ConfigParseError, e.what()};
    }
    catch (const toml::file_io_error &e)
    {
        return PulseError{ErrorCode::ConfigFileNotFound, e.what()};
    }

    // Stage 3: Resolve from_env: values.
    auto env_err = resolveEnvVars(root, "");

    if (ErrorCode::Ok != env_err.code)
    {
        return env_err;
    }

    // Stage 4: Map TOML sections to config structs.
    PulseConfig cfg; // All fields start with config.hpp defaults.

    // Each parser returns early if its section is absent.
    auto err = parseExchange(root, cfg.exchange);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseLog(root, cfg.log);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseSymbols(root, cfg.symbols);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseAi(root, cfg.ai);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseTwitter(root, cfg.twitter);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseNews(root, cfg.news);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseRisk(root, cfg.risk);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseStrategy(root, cfg.strategy);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseControl(root, cfg.control);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseSqlite(root, cfg.sqlite);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parseGrid(root, cfg.grid);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    // Top-level default_market_type.
    if (root.contains("default_market_type"))
    {
        std::string mt_str = toml::find<std::string>(root, "default_market_type");
        err = parseMarketType(mt_str, cfg.default_market_type);

        if (ErrorCode::Ok != err.code)
        {
            return err;
        }
    }

    // Top-level active_market — the single trading direction active at startup.
    if (root.contains("active_market"))
    {
        std::string am_str = toml::find<std::string>(root, "active_market");
        err = parseMarketType(am_str, cfg.active_market);

        if (ErrorCode::Ok != err.code)
        {
            return err;
        }
    }

    return cfg;
}

} // namespace pulse
