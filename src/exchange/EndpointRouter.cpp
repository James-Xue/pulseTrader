// endpoint_router.cpp — Market-type-aware routing for Gate.io REST/WS endpoints (Layer 1 Exchange)
//
// Pure-function implementation — no state, no side effects.
// All routing logic is centralised here so the rest of the exchange layer
// never hardcodes "spot" or "futures" path fragments.

#include "exchange/EndpointRouter.hpp"

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// restPrefix
// ---------------------------------------------------------------------------
std::string EndpointRouter::restPrefix(MarketType mt)
{
    switch (mt)
    {
    case MarketType::Spot:
        return "/api/v4/spot";
    case MarketType::Futures:
        return "/api/v4/futures/usdt";
    case MarketType::Cfd:
        return "/api/v4/tradfi";
    }
    return "/api/v4/spot";
}

// ---------------------------------------------------------------------------
// wsPrefix
// ---------------------------------------------------------------------------
std::string EndpointRouter::wsPrefix(MarketType mt)
{
    switch (mt)
    {
    case MarketType::Spot:
        return "spot";
    case MarketType::Futures:
        return "futures";
    case MarketType::Cfd:
        return "cfd"; // No WS channels exist for TradFi — defensive only.
    }
    return "spot";
}

// ---------------------------------------------------------------------------
// wsChannel — build full channel name from prefix + suffix
// ---------------------------------------------------------------------------
std::string EndpointRouter::wsChannel(MarketType mt, std::string_view suffix)
{
    std::string result = wsPrefix(mt);
    result += '.';
    result += suffix;
    return result;
}

// ---------------------------------------------------------------------------
// pingChannel
// ---------------------------------------------------------------------------
std::string EndpointRouter::pingChannel(MarketType mt)
{
    return wsChannel(mt, "ping");
}

// ---------------------------------------------------------------------------
// pongChannel
// ---------------------------------------------------------------------------
std::string EndpointRouter::pongChannel(MarketType mt)
{
    return wsChannel(mt, "pong");
}

// ---------------------------------------------------------------------------
// selectWsUrl
// ---------------------------------------------------------------------------
std::string EndpointRouter::selectWsUrl(const ExchangeConfig &cfg, MarketType mt)
{
    switch (mt)
    {
    case MarketType::Spot:
        return cfg.wsUrl;
    case MarketType::Futures:
        return cfg.futuresWsUrl;
    case MarketType::Cfd:
        return cfg.wsUrl; // Unused — CFD has no WebSocket market data.
    }
    return cfg.wsUrl;
}

// ---------------------------------------------------------------------------
// needsJsonPing
// ---------------------------------------------------------------------------
bool EndpointRouter::needsJsonPing(MarketType mt)
{
    // Spot: server sends JSON {"channel": "spot.ping"} — we must reply with spot.pong.
    // Futures: server uses RFC 6455 protocol-layer ping — websocketpp auto-responds.
    //          However, if a JSON futures.ping ever arrives, the handler will still respond.
    return MarketType::Spot == mt;
}

// ---------------------------------------------------------------------------
// REST endpoint builders
// ---------------------------------------------------------------------------
std::string EndpointRouter::contractsPath(MarketType mt)
{
    switch (mt)
    {
    case MarketType::Spot:
        return "/api/v4/spot/currency_pairs";
    case MarketType::Futures:
        return "/api/v4/futures/usdt/contracts";
    case MarketType::Cfd:
        return "/api/v4/tradfi/symbols";
    }
    return "/api/v4/spot/currency_pairs";
}

std::string EndpointRouter::tickersPath(MarketType mt)
{
    return restPrefix(mt) + "/tickers";
}

std::string EndpointRouter::accountsPath(MarketType mt)
{
    if (MarketType::Cfd == mt)
    {
        return "/api/v4/tradfi/users/assets"; // TradFi balances live under /users/, not /accounts.
    }
    return restPrefix(mt) + "/accounts";
}

// ---------------------------------------------------------------------------
// ordersPath
// ---------------------------------------------------------------------------
std::string EndpointRouter::ordersPath(MarketType mt)
{
    return restPrefix(mt) + "/orders";
}

// ---------------------------------------------------------------------------
// orderPath — specific order by ID
// ---------------------------------------------------------------------------
std::string EndpointRouter::orderPath(MarketType mt, const std::string &order_id)
{
    return restPrefix(mt) + "/orders/" + order_id;
}

// ---------------------------------------------------------------------------
// positionsPath — futures only (spot has no position endpoint; CFD has its
// own getCfdPositions() with a different response shape)
// ---------------------------------------------------------------------------
std::string EndpointRouter::positionsPath(MarketType mt)
{
    if (MarketType::Futures == mt)
    {
        return "/api/v4/futures/usdt/positions";
    }
    return "";
}

// ---------------------------------------------------------------------------
// leveragePath — futures only
// ---------------------------------------------------------------------------
std::string EndpointRouter::leveragePath(MarketType mt, const std::string &contract)
{
    switch (mt)
    {
    case MarketType::Spot:
        return ""; // Not applicable for spot.
    case MarketType::Futures:
        return "/api/v4/futures/usdt/positions/" + contract + "/leverage";
    case MarketType::Cfd:
        return ""; // CFD leverage is account-level, not per-position.
    }
    return "";
}

// ---------------------------------------------------------------------------
// TradFi (CFD) specific endpoint builders
// ---------------------------------------------------------------------------
std::string EndpointRouter::cfdSymbolsPath()
{
    return "/api/v4/tradfi/symbols";
}

std::string EndpointRouter::cfdSymbolsDetailPath()
{
    return "/api/v4/tradfi/symbols/detail";
}

std::string EndpointRouter::cfdTickerPath(const std::string &symbol)
{
    return "/api/v4/tradfi/symbols/" + symbol + "/tickers";
}

std::string EndpointRouter::cfdKlinesPath(const std::string &symbol)
{
    return "/api/v4/tradfi/symbols/" + symbol + "/klines";
}

std::string EndpointRouter::cfdAssetsPath()
{
    return "/api/v4/tradfi/users/assets";
}

std::string EndpointRouter::cfdPositionsPath()
{
    return "/api/v4/tradfi/positions";
}

std::string EndpointRouter::cfdPositionClosePath(const std::string &position_id)
{
    return "/api/v4/tradfi/positions/" + position_id + "/close";
}

std::string EndpointRouter::cfdPositionModifyPath(const std::string &position_id)
{
    // PUT /tradfi/positions/{id} — modify an open CFD position (dynamic
    // SL/TP adjustment: request body takes price_sl / price_tp).
    return "/api/v4/tradfi/positions/" + position_id;
}

} // namespace pulse::exchange
