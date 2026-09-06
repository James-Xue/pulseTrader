// quanto_resolver.cpp — Futures contract metadata for any coin (M33, A1)

#include "backtest/QuantoResolver.hpp"

#include "exchange/GateRestClient.hpp"
#include "logging/Logger.hpp"
#include "market/SymbolRegistry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace pulse::backtest
{

namespace
{

/// Cache freshness window: 12 hours of cached contract metadata. Long
/// enough to make parameter sweeps affordable (one fetch per run would
/// otherwise re-pull the whole contract list every time).
constexpr std::int64_t kCacheTtlMs = 12LL * 3600 * 1000;

/// SymbolInfo → cache JSON subset.
nlohmann::json symbolToJson(const market::SymbolInfo &info)
{
    return {
        { "quanto_multiplier", info.quanto_multiplier },
        { "tick_size", info.tick_size },
        { "leverage_max", info.leverage_max },
        { "order_size_min", info.order_size_min },
        { "order_size_max", info.order_size_max },
    };
}

/// Cache JSON subset → SymbolInfo (market_type = Futures).
market::SymbolInfo symbolFromJson(const std::string &symbol, const nlohmann::json &j)
{
    market::SymbolInfo info;
    info.symbol = symbol;
    info.market_type = MarketType::Futures;
    info.quanto_multiplier = j.value("quanto_multiplier", 1.0);
    info.tick_size = j.value("tick_size", 0.0);
    info.leverage_max = j.value("leverage_max", 1.0);
    info.order_size_min = j.value("order_size_min", 0);
    info.order_size_max = j.value("order_size_max", 0);
    return info;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

QuantoResolver::QuantoResolver(exchange::GateRestClient &rest, std::string cache_path)
    : m_rest{ rest }
    , m_cachePath{ std::move(cache_path) }
{
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result<double> QuantoResolver::resolveQuanto(
    const std::string &symbol, MarketType market_type)
{
    if (MarketType::Spot == market_type)
    {
        return 1.0;
    }
    if (MarketType::Cfd == market_type)
    {
        return PulseError{ ErrorCode::BacktestConfigInvalid,
            "CFD backtests are unsupported: " + symbol
                + " has no REST kline source with a time range" };
    }

    auto meta = futuresContract(symbol);
    if (!ok(meta))
    {
        return error(meta);
    }
    return value(meta).quanto_multiplier;
}

Result<market::SymbolInfo> QuantoResolver::futuresContract(const std::string &symbol)
{
    auto loaded = ensureLoaded();
    if (!ok(loaded))
    {
        return error(loaded);
    }

    const auto it = m_contracts.find(symbol);
    if (m_contracts.end() == it)
    {
        // Did-you-mean: sorted sample of up to 15 contract names.
        std::vector<std::string> names;
        names.reserve(m_contracts.size());
        for (const auto &[name, info] : m_contracts)
        {
            (void)info;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        std::string sample;
        for (std::size_t i = 0; i < names.size() && i < 15; ++i)
        {
            sample += (sample.empty() ? "" : ", ") + names[i];
        }

        return PulseError{ ErrorCode::BacktestContractUnknown,
            "Unknown futures contract '" + symbol + "' (" + std::to_string(names.size())
                + " contracts available; e.g. " + sample
                + (names.size() > 15 ? ", ..." : "")
                + "). Pass --quanto Q to override contract resolution." };
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// Cache load / refresh
// ---------------------------------------------------------------------------

Result<bool> QuantoResolver::ensureLoaded()
{
    if (cacheFresh())
    {
        return true;
    }

    // A stale or missing in-memory cache first tries the on-disk copy.
    if (m_contracts.empty())
    {
        loadCacheFromFile();
        if (cacheFresh())
        {
            return true;
        }
    }

    // Cache miss → fetch the full futures contract list (public REST).
    PULSE_LOG_INFO("backtest", "Fetching futures contract list from Gate REST");
    market::SymbolRegistry registry(m_rest, MarketType::Futures);
    if (!registry.loadFromRest())
    {
        m_contracts.clear();
        return PulseError{ ErrorCode::BacktestContractFetchFailed,
            "Could not fetch the futures contract list from Gate REST"
            + (m_cachePath.empty()
                   ? std::string{}
                   : " (no cached copy at " + m_cachePath + ")")
            + " — pass --quanto Q to skip contract resolution" };
    }

    m_contracts.clear();
    for (const auto &name : registry.symbols())
    {
        const auto info = registry.get(name);
        if (info.has_value())
        {
            m_contracts[name] = *info;
        }
    }
    m_fetchedAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    saveCacheToFile();
    PULSE_LOG_INFO("backtest", "Loaded {} futures contracts", m_contracts.size());
    return true;
}

bool QuantoResolver::cacheFresh() const
{
    if (m_contracts.empty() || 0 == m_fetchedAtMs)
    {
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now - m_fetchedAtMs) < kCacheTtlMs;
}

void QuantoResolver::loadCacheFromFile()
{
    if (m_cachePath.empty())
    {
        return;
    }
    try
    {
        std::ifstream in(m_cachePath);
        if (!in)
        {
            return; // no cache yet — fetch below.
        }
        const auto root = nlohmann::json::parse(in);
        m_fetchedAtMs = root.value("fetched_at_ms", std::int64_t{ 0 });
        const auto &futures = root.value("futures", nlohmann::json::object());
        for (auto it = futures.begin(); it != futures.end(); ++it)
        {
            m_contracts[it.key()] = symbolFromJson(it.key(), it.value());
        }
        if (m_contracts.empty())
        {
            m_fetchedAtMs = 0;
        }
    }
    catch (const nlohmann::json::exception &e)
    {
        PULSE_LOG_WARN("backtest", "Contract cache at {} unreadable ({}): "
                       "refetching", m_cachePath, e.what());
        m_contracts.clear();
        m_fetchedAtMs = 0;
    }
}

void QuantoResolver::saveCacheToFile()
{
    if (m_cachePath.empty())
    {
        return;
    }
    try
    {
        nlohmann::json futures = nlohmann::json::object();
        for (const auto &[name, info] : m_contracts)
        {
            futures[name] = symbolToJson(info);
        }
        const nlohmann::json root = {
            { "fetched_at_ms", m_fetchedAtMs },
            { "futures", futures },
        };

        // tmp + rename keeps the cache atomic across crashes.
        const std::string tmp_path = m_cachePath + ".tmp";
        {
            std::ofstream out(tmp_path);
            if (!out)
            {
                PULSE_LOG_WARN("backtest", "Cannot write contract cache to {}",
                               tmp_path);
                return;
            }
            out << root.dump(2);
        }
        if (0 != std::rename(tmp_path.c_str(), m_cachePath.c_str()))
        {
            PULSE_LOG_WARN("backtest", "Cannot rename contract cache {} → {}",
                           tmp_path, m_cachePath);
            std::remove(tmp_path.c_str());
        }
    }
    catch (const std::exception &e)
    {
        PULSE_LOG_WARN("backtest", "Contract cache write failed: {}", e.what());
    }
}

} // namespace pulse::backtest
