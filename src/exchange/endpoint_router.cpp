// endpoint_router.cpp — Market-type-aware routing for Gate.io REST/WS endpoints (Layer 1 Exchange)
//
// Pure-function implementation — no state, no side effects.
// All routing logic is centralised here so the rest of the exchange layer
// never hardcodes "spot" or "futures" path fragments.

#include "exchange/endpoint_router.hpp"

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// rest_prefix
// ---------------------------------------------------------------------------
std::string EndpointRouter::rest_prefix(MarketType mt)
{
    switch (mt)
    {
    case MarketType::Spot:
        return "/api/v4/spot";
    case MarketType::Futures:
        return "/api/v4/futures/usdt";
    }
    return "/api/v4/spot";
}

// ---------------------------------------------------------------------------
// ws_prefix
// ---------------------------------------------------------------------------
std::string EndpointRouter::ws_prefix(MarketType mt)
{
    switch (mt)
    {
    case MarketType::Spot:
        return "spot";
    case MarketType::Futures:
        return "futures";
    }
    return "spot";
}

// ---------------------------------------------------------------------------
// ws_channel — build full channel name from prefix + suffix
// ---------------------------------------------------------------------------
std::string EndpointRouter::ws_channel(MarketType mt, std::string_view suffix)
{
    std::string result = ws_prefix(mt);
    result += '.';
    result += suffix;
    return result;
}

// ---------------------------------------------------------------------------
// ping_channel
// ---------------------------------------------------------------------------
std::string EndpointRouter::ping_channel(MarketType mt)
{
    return ws_channel(mt, "ping");
}

// ---------------------------------------------------------------------------
// pong_channel
// ---------------------------------------------------------------------------
std::string EndpointRouter::pong_channel(MarketType mt)
{
    return ws_channel(mt, "pong");
}

// ---------------------------------------------------------------------------
// select_ws_url
// ---------------------------------------------------------------------------
std::string EndpointRouter::select_ws_url(const ExchangeConfig &cfg, MarketType mt)
{
    switch (mt)
    {
    case MarketType::Spot:
        return cfg.wsUrl;
    case MarketType::Futures:
        return cfg.futuresWsUrl;
    }
    return cfg.wsUrl;
}

// ---------------------------------------------------------------------------
// needs_json_ping
// ---------------------------------------------------------------------------
bool EndpointRouter::needs_json_ping(MarketType mt)
{
    // Spot: server sends JSON {"channel": "spot.ping"} — we must reply with spot.pong.
    // Futures: server uses RFC 6455 protocol-layer ping — websocketpp auto-responds.
    //          However, if a JSON futures.ping ever arrives, the handler will still respond.
    return MarketType::Spot == mt;
}

// ---------------------------------------------------------------------------
// REST endpoint builders
// ---------------------------------------------------------------------------
std::string EndpointRouter::contracts_path(MarketType mt)
{
    switch (mt)
    {
    case MarketType::Spot:
        return "/api/v4/spot/currency_pairs";
    case MarketType::Futures:
        return "/api/v4/futures/usdt/contracts";
    }
    return "/api/v4/spot/currency_pairs";
}

std::string EndpointRouter::tickers_path(MarketType mt)
{
    return rest_prefix(mt) + "/tickers";
}

std::string EndpointRouter::accounts_path(MarketType mt)
{
    return rest_prefix(mt) + "/accounts";
}

} // namespace pulse::exchange
