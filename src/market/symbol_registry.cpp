// symbolRegistry.cpp — SymbolRegistry implementation (Layer 3 Market Data)

#include "market/symbol_registry.hpp"

#include "logging/logger.hpp"

#include <cmath>

namespace pulse::market
{

using namespace pulse::logging;

SymbolRegistry::SymbolRegistry(exchange::GateRestClient &rest_client, MarketType market_type)
    : m_restClient{ rest_client }
    , m_marketType{ market_type }
{
}

bool SymbolRegistry::loadFromRest()
{
    // Fetch instruments based on market type.
    nlohmann::json instruments_json;

    if (MarketType::Futures == m_marketType)
    {
        auto result = m_restClient.getFuturesContracts();
        if (!pulse::ok(result))
        {
            PULSE_LOG_WARN("market", "Failed to fetch futures contracts: {}", pulse::error(result).message);
            return false;
        }
        instruments_json = pulse::value(result);
    }
    else
    {
        auto result = m_restClient.getCurrencyPairs();
        if (!pulse::ok(result))
        {
            PULSE_LOG_WARN("market", "Failed to fetch currency pairs: {}", pulse::error(result).message);
            return false;
        }
        instruments_json = pulse::value(result);
    }

    if (!instruments_json.is_array())
    {
        PULSE_LOG_WARN("market", "Instruments response is not an array");
        return false;
    }

    // Parse each instrument and build the registry.
    std::unordered_map<Symbol, SymbolInfo> new_symbols;
    for (const auto &obj : instruments_json)
    {
        std::optional<SymbolInfo> info_opt;
        if (MarketType::Futures == m_marketType)
        {
            info_opt = parseFuturesContract(obj);
        }
        else
        {
            info_opt = parseCurrencyPair(obj);
        }

        if (info_opt.has_value())
        {
            new_symbols.emplace(info_opt->symbol, std::move(*info_opt));
        }
    }

    // Replace the registry atomically (exclusive lock).
    std::unique_lock<std::shared_mutex> write_lock(m_mutex);
    m_symbols = std::move(new_symbols);

    PULSE_LOG_INFO("market", "Loaded {} {} instruments from REST",
                   m_symbols.size(),
                   MarketType::Futures == m_marketType ? "futures" : "spot");
    return true;
}

std::optional<SymbolInfo> SymbolRegistry::get(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    const auto it = m_symbols.find(symbol);
    if (it == m_symbols.end())
    {
        return std::nullopt;
    }
    return it->second;
}

bool SymbolRegistry::validateOrder(const Symbol &symbol, Price price, Quantity qty) const
{
    const auto info_opt = get(symbol);
    if (!info_opt.has_value())
    {
        return false; // Symbol not found.
    }

    const auto &info = *info_opt;

    // Check trading is enabled.
    if (!info.trading_enabled)
    {
        return false;
    }

    // Futures-specific validation.
    if (MarketType::Futures == info.market_type)
    {
        // Quantity must be a whole number of contracts.
        if (std::abs(qty - std::round(qty)) > 1e-9)
        {
            return false;
        }

        // Check minimum order size.
        if (info.order_size_min > 0 && qty < info.order_size_min)
        {
            return false;
        }

        // Check maximum order size.
        if (info.order_size_max > 0 && qty > info.order_size_max)
        {
            return false;
        }

        // Check price is a multiple of tick_size (with floating-point tolerance).
        if (info.tick_size > 0.0 && price > 0.0)
        {
            const double price_remainder = std::fmod(price, info.tick_size);
            const double tolerance = info.tick_size * 1e-6;
            if (std::abs(price_remainder) > tolerance && std::abs(price_remainder - info.tick_size) > tolerance)
            {
                return false;
            }
        }

        return true;
    }

    // Spot validation (unchanged).

    // Check price is a multiple of tick_size (with floating-point tolerance).
    if (info.tick_size > 0.0)
    {
        const double price_remainder = std::fmod(price, info.tick_size);
        const double tolerance = info.tick_size * 1e-6;
        if (std::abs(price_remainder) > tolerance && std::abs(price_remainder - info.tick_size) > tolerance)
        {
            return false;
        }
    }

    // Check quantity is a multiple of lot_size.
    if (info.lot_size > 0.0)
    {
        const double qty_remainder = std::fmod(qty, info.lot_size);
        const double tolerance = info.lot_size * 1e-6;
        if (std::abs(qty_remainder) > tolerance && std::abs(qty_remainder - info.lot_size) > tolerance)
        {
            return false;
        }
    }

    // Check base amount >= min_base_amount.
    if (qty < info.min_base_amount)
    {
        return false;
    }

    // Check quote amount >= min_quote_amount.
    const double quote_amount = price * qty;
    if (quote_amount < info.min_quote_amount)
    {
        return false;
    }

    // Check notional >= min_notional.
    if (quote_amount < info.min_notional)
    {
        return false;
    }

    return true;
}

std::size_t SymbolRegistry::size() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    return m_symbols.size();
}

std::vector<Symbol> SymbolRegistry::symbols() const
{
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    std::vector<Symbol> result;
    result.reserve(m_symbols.size());
    for (const auto &[sym, _] : m_symbols)
    {
        result.push_back(sym);
    }
    return result;
}

std::optional<SymbolInfo> SymbolRegistry::parseCurrencyPair(const nlohmann::json &obj)
{
    // Gate.io format: see header comment.
    // Required fields: id, precision, amount_precision, trade_status.
    // Optional fields: min_base_amount, min_quote_amount.

    if (!obj.contains("id") || !obj.contains("precision") || !obj.contains("amount_precision"))
    {
        return std::nullopt;
    }

    SymbolInfo info;
    info.symbol = obj["id"].get<std::string>();

    // Parse tick size from "precision" field (number of decimal places).
    const int price_precision = obj["precision"].get<int>();
    info.tick_size = std::pow(10.0, -price_precision);

    // Parse lot size from "amount_precision" field.
    const int amount_precision = obj["amount_precision"].get<int>();
    info.lot_size = std::pow(10.0, -amount_precision);

    // Parse min_base_amount (optional, default 0).
    if (obj.contains("min_base_amount") && !obj["min_base_amount"].is_null())
    {
        info.min_base_amount = safeParseDouble(obj["min_base_amount"].get<std::string>()).value_or(0.0);
    }

    // Parse min_quote_amount (optional, default 0).
    if (obj.contains("min_quote_amount") && !obj["min_quote_amount"].is_null())
    {
        info.min_quote_amount = safeParseDouble(obj["min_quote_amount"].get<std::string>()).value_or(0.0);
    }

    // min_notional = max(min_quote_amount, price * min_base_amount)
    // For simplicity, use min_quote_amount as min_notional.
    info.min_notional = info.min_quote_amount;

    // Parse trade_status (optional, default "tradable").
    info.trading_enabled = true;
    if (obj.contains("trade_status"))
    {
        const auto status = obj["trade_status"].get<std::string>();
        info.trading_enabled = (status == "tradable");
    }

    return info;
}

std::optional<SymbolInfo> SymbolRegistry::parseFuturesContract(const nlohmann::json &obj)
{
    // Gate.io futures contract format:
    // Required fields: name, quanto_multiplier, leverage_max.
    // Optional fields: leverage_min, maintenance_rate, funding_interval,
    //                  order_size_min, order_size_max, order_price_round.

    if (!obj.contains("name"))
    {
        return std::nullopt;
    }

    SymbolInfo info;
    info.symbol = obj["name"].get<std::string>();
    info.market_type = MarketType::Futures;
    info.trading_enabled = true; // Contracts returned by API are tradable.

    // Contract multiplier (how much base asset one contract represents).
    if (obj.contains("quanto_multiplier") && !obj["quanto_multiplier"].is_null())
    {
        info.quanto_multiplier = safeParseDouble(obj["quanto_multiplier"].get<std::string>()).value_or(1.0);
    }

    // Leverage bounds.
    if (obj.contains("leverage_max") && !obj["leverage_max"].is_null())
    {
        info.leverage_max = safeParseDouble(obj["leverage_max"].get<std::string>()).value_or(1.0);
    }

    if (obj.contains("leverage_min") && !obj["leverage_min"].is_null())
    {
        info.leverage_min = safeParseDouble(obj["leverage_min"].get<std::string>()).value_or(1.0);
    }

    // Maintenance margin rate.
    if (obj.contains("maintenance_rate") && !obj["maintenance_rate"].is_null())
    {
        info.maintenance_rate = safeParseDouble(obj["maintenance_rate"].get<std::string>()).value_or(0.0);
    }

    // Funding interval in seconds.
    if (obj.contains("funding_interval"))
    {
        info.funding_interval = obj["funding_interval"].get<int>();
    }

    // Order size bounds (in contracts).
    if (obj.contains("order_size_min"))
    {
        info.order_size_min = obj["order_size_min"].get<int>();
    }

    if (obj.contains("order_size_max"))
    {
        info.order_size_max = obj["order_size_max"].get<int>();
    }

    // Price precision — order_price_round is the minimum price increment.
    if (obj.contains("order_price_round") && !obj["order_price_round"].is_null())
    {
        info.tick_size = safeParseDouble(obj["order_price_round"].get<std::string>()).value_or(0.0);
    }

    // Contracts trade in whole units.
    info.lot_size = 1.0;

    // Futures min_notional is derived from order_size_min * quanto_multiplier * price.
    // Set to 0 here; validateOrder() handles size checks directly.
    info.min_notional = 0.0;
    info.min_quote_amount = 0.0;
    info.min_base_amount = 0.0;

    return info;
}

} // namespace pulse::market
