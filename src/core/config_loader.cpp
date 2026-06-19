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
// resolve_env_vars — recursively walk TOML tree, replacing "from_env:VAR"
// ---------------------------------------------------------------------------
PulseError resolve_env_vars(toml::value &node, const std::string &path)
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
            // Runtime validation (validate_config) will catch missing
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
            auto err = resolve_env_vars(child, child_path);

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
            auto err = resolve_env_vars(arr[i], child_path);

            if (ErrorCode::Ok != err.code)
            {
                return err;
            }
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// find_double — extract a double from TOML, accepting both integer and float
//
// toml11 v4 distinguishes TOML integer (int64_t) from float (double).
// toml::find_or<double>() fails if the TOML value is an integer (e.g. 500
// instead of 500.0). This helper handles both cases.
// ---------------------------------------------------------------------------
double find_double(const toml::value &tbl, const std::string &key,
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
// parse_stop_mode — string to StopMode enum
// ---------------------------------------------------------------------------
PulseError parse_stop_mode(const std::string &str, StopMode &out)
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
// Section parsers — each reads one TOML [section] into a config struct
// ---------------------------------------------------------------------------

PulseError parse_exchange(const toml::value &root, ExchangeConfig &out)
{
    if (!root.contains("exchange"))
    {
        return {};
    }

    const auto &sec = root.at("exchange");

    out.apiKey = toml::find_or(sec, "apiKey", out.apiKey);
    out.apiSecret = toml::find_or(sec, "apiSecret", out.apiSecret);
    out.restBaseUrl = toml::find_or(sec, "restBaseUrl", out.restBaseUrl);
    out.wsUrl = toml::find_or(sec, "wsUrl", out.wsUrl);
    out.proxyUrl = toml::find_or(sec, "proxyUrl", out.proxyUrl);
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

PulseError parse_log(const toml::value &root, LogConfig &out)
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

PulseError parse_symbols(const toml::value &root,
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

PulseError parse_ai(const toml::value &root, AiConfig &out)
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

PulseError parse_twitter(const toml::value &root, TwitterConfig &out)
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

PulseError parse_news(const toml::value &root, NewsConfig &out)
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

PulseError parse_stop_loss(const toml::value &sec, StopLossConfig &out)
{
    if (!sec.contains("stop_loss"))
    {
        return {};
    }

    const auto &sub = sec.at("stop_loss");

    if (sub.contains("mode"))
    {
        std::string mode_str = toml::find<std::string>(sub, "mode");
        auto err = parse_stop_mode(mode_str, out.mode);

        if (ErrorCode::Ok != err.code)
        {
            return err;
        }
    }

    out.fixed_pct = find_double(sub, "fixed_pct", out.fixed_pct);
    out.trailing_pct = find_double(sub, "trailing_pct", out.trailing_pct);
    out.max_hold_seconds =
        static_cast<std::uint32_t>(
            toml::find_or(sub, "max_hold_seconds",
                          static_cast<int>(out.max_hold_seconds)));

    return {};
}

PulseError parse_take_profit(const toml::value &sec, TakeProfitConfig &out)
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

PulseError parse_risk(const toml::value &root, RiskConfig &out)
{
    if (!root.contains("risk"))
    {
        return {};
    }

    const auto &sec = root.at("risk");

    out.maxPositionNotional =
        find_double(sec, "maxPositionNotional", out.maxPositionNotional);
    out.maxOpenPositions =
        toml::find_or(sec, "maxOpenPositions", out.maxOpenPositions);
    out.maxDailyDrawdown =
        find_double(sec, "maxDailyDrawdown", out.maxDailyDrawdown);
    out.maxDrawdown =
        find_double(sec, "maxDrawdown", out.maxDrawdown);
    out.maxOrdersPerSec =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "maxOrdersPerSec",
                          static_cast<int>(out.maxOrdersPerSec)));
    out.maxSymbolNotional =
        find_double(sec, "maxSymbolNotional", out.maxSymbolNotional);

    // Nested sub-tables.
    auto err = parse_stop_loss(sec, out.stop_loss);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_take_profit(sec, out.take_profit);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    return {};
}

PulseError parse_strategy_instance(const toml::value &tbl,
                                   StrategyInstanceConfig &out)
{
    out.name = toml::find_or(tbl, "name", out.name);
    out.symbol = toml::find_or(tbl, "symbol", out.symbol);
    out.order_quantity =
        find_double(tbl, "order_quantity", out.order_quantity);
    out.min_confidence =
        find_double(tbl, "min_confidence", out.min_confidence);
    out.enabled = toml::find_or(tbl, "enabled", out.enabled);
    out.poll_interval_ms =
        static_cast<std::uint32_t>(
            toml::find_or(tbl, "poll_interval_ms",
                          static_cast<int>(out.poll_interval_ms)));

    return {};
}

PulseError parse_strategy(const toml::value &root, StrategyConfig &out)
{
    if (!root.contains("strategy"))
    {
        return {};
    }

    const auto &sec = root.at("strategy");

    out.signal_aggregator_threshold = find_double(
        sec, "signal_aggregator_threshold",
        out.signal_aggregator_threshold);
    out.signal_cooldown_sec =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "signal_cooldown_sec",
                          static_cast<int>(out.signal_cooldown_sec)));

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
            auto err = parse_strategy_instance(elem, inst);

            if (ErrorCode::Ok != err.code)
            {
                return err;
            }

            out.strategies.push_back(std::move(inst));
        }
    }

    return {};
}

PulseError parse_webui(const toml::value &root, WebUiConfig &out)
{
    if (!root.contains("webui"))
    {
        return {};
    }

    const auto &sec = root.at("webui");

    out.enabled = toml::find_or(sec, "enabled", out.enabled);
    out.bindAddress = toml::find_or(sec, "bindAddress", out.bindAddress);
    out.port = static_cast<std::uint16_t>(
        toml::find_or(sec, "port", static_cast<int>(out.port)));
    out.authToken = toml::find_or(sec, "authToken", out.authToken);
    out.maxClients =
        static_cast<std::uint32_t>(
            toml::find_or(sec, "maxClients",
                          static_cast<int>(out.maxClients)));

    return {};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<PulseConfig> load_config_file(const std::filesystem::path &path)
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
    auto env_err = resolve_env_vars(root, "");

    if (ErrorCode::Ok != env_err.code)
    {
        return env_err;
    }

    // Stage 4: Map TOML sections to config structs.
    PulseConfig cfg; // All fields start with config.hpp defaults.

    // Each parser returns early if its section is absent.
    auto err = parse_exchange(root, cfg.exchange);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_log(root, cfg.log);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_symbols(root, cfg.symbols);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_ai(root, cfg.ai);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_twitter(root, cfg.twitter);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_news(root, cfg.news);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_risk(root, cfg.risk);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_strategy(root, cfg.strategy);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    err = parse_webui(root, cfg.webui);

    if (ErrorCode::Ok != err.code)
    {
        return err;
    }

    return cfg;
}

} // namespace pulse
