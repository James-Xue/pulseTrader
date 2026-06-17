// symbol_registry.cpp — SymbolRegistry implementation (Layer 3 Market Data)

#include "pulse/market/symbol_registry.hpp"

#include "pulse/logging/logger.hpp"

#include <cmath>

namespace pulse::market
{

using namespace pulse::logging;

SymbolRegistry::SymbolRegistry(exchange::GateRestClient &rest_client)
    : rest_client_{ rest_client }
{
}

bool SymbolRegistry::load_from_rest()
{
    // Fetch all currency pairs from Gate.io.
    auto result = rest_client_.get_currency_pairs();
    if (!pulse::ok(result))
    {
        PULSE_LOG_WARN("market", "Failed to fetch currency pairs: {}", pulse::error(result).message);
        return false;
    }

    const auto &pairs_json = pulse::value(result);
    if (!pairs_json.is_array())
    {
        PULSE_LOG_WARN("market", "Currency pairs response is not an array");
        return false;
    }

    // Parse each currency pair and build the registry.
    std::unordered_map<Symbol, SymbolInfo> new_symbols;
    for (const auto &pair_obj : pairs_json)
    {
        auto info_opt = parse_currency_pair(pair_obj);
        if (info_opt.has_value())
        {
            new_symbols.emplace(info_opt->symbol, std::move(*info_opt));
        }
    }

    // Replace the registry atomically (exclusive lock).
    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    symbols_ = std::move(new_symbols);

    PULSE_LOG_INFO("market", "Loaded {} currency pairs from REST", symbols_.size());
    return true;
}

std::optional<SymbolInfo> SymbolRegistry::get(const Symbol &symbol) const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    const auto it = symbols_.find(symbol);
    if (it == symbols_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

bool SymbolRegistry::validate_order(const Symbol &symbol, Price price, Quantity qty) const
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
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    return symbols_.size();
}

std::vector<Symbol> SymbolRegistry::symbols() const
{
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    std::vector<Symbol> result;
    result.reserve(symbols_.size());
    for (const auto &[sym, _] : symbols_)
    {
        result.push_back(sym);
    }
    return result;
}

std::optional<SymbolInfo> SymbolRegistry::parse_currency_pair(const nlohmann::json &obj)
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
        info.min_base_amount = std::stod(obj["min_base_amount"].get<std::string>());
    }

    // Parse min_quote_amount (optional, default 0).
    if (obj.contains("min_quote_amount") && !obj["min_quote_amount"].is_null())
    {
        info.min_quote_amount = std::stod(obj["min_quote_amount"].get<std::string>());
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

} // namespace pulse::market
