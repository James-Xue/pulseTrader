#pragma once
// endpoint_router.hpp — Market-type-aware routing for Gate.io REST/WS endpoints (Layer 1 Exchange)
//
// Pure-function utility class that maps MarketType to the correct:
//   1. REST API path prefix (spot vs futures)
//   2. WebSocket channel prefix (spot.* vs futures.*)
//   3. Ping/pong channel names
//   4. WebSocket URL selection
//   5. REST endpoint builders for common queries
//
// All methods are static and stateless — no instance required.
// This enables the exchange layer to support both spot and futures markets
// through a single code path parameterised by MarketType.

#include "core/config.hpp"
#include "core/types.hpp"

#include <string>
#include <string_view>

namespace pulse::exchange
{

// ---------------------------------------------------------------------------
// EndpointRouter — static routing functions for Gate.io spot and futures APIs
//
// Gate.io API structure:
//   Spot REST:    https://api.gateio.ws/api/v4/spot/...
//   Futures REST: https://api.gateio.ws/api/v4/futures/usdt/...
//   Spot WS:      wss://api.gateio.ws/ws/v4/     (channels: spot.tickers, spot.ping, ...)
//   Futures WS:   wss://fx-ws.gateio.ws/v4/ws/usdt (channels: futures.tickers, futures.ping, ...)
// ---------------------------------------------------------------------------
class EndpointRouter
{
  public:
    /// REST API path prefix for the given market type.
    ///
    /// Spot:    "/api/v4/spot"
    /// Futures: "/api/v4/futures/usdt"
    [[nodiscard]] static std::string rest_prefix(MarketType mt);

    /// WebSocket channel name prefix for the given market type.
    ///
    /// Spot:    "spot"
    /// Futures: "futures"
    [[nodiscard]] static std::string ws_prefix(MarketType mt);

    /// Build a full WebSocket channel name: ws_prefix(mt) + "." + suffix.
    ///
    /// Examples:
    ///   ws_channel(Spot, "tickers")    → "spot.tickers"
    ///   ws_channel(Futures, "tickers") → "futures.tickers"
    [[nodiscard]] static std::string ws_channel(MarketType mt, std::string_view suffix);

    /// Server-initiated ping channel name.
    ///
    /// Spot:    "spot.ping"
    /// Futures: "futures.ping"
    [[nodiscard]] static std::string ping_channel(MarketType mt);

    /// Client reply pong channel name.
    ///
    /// Spot:    "spot.pong"
    /// Futures: "futures.pong"
    [[nodiscard]] static std::string pong_channel(MarketType mt);

    /// Select the appropriate WebSocket URL from ExchangeConfig.
    ///
    /// Spot:    config.wsUrl    (wss://api.gateio.ws/ws/v4/)
    /// Futures: config.futuresWsUrl (wss://fx-ws.gateio.ws/v4/ws/usdt)
    [[nodiscard]] static std::string select_ws_url(const ExchangeConfig &cfg, MarketType mt);

    /// Whether the given market type uses JSON application-layer ping/pong.
    ///
    /// Spot uses JSON ping/pong (spot.ping → spot.pong).
    /// Futures uses RFC 6455 protocol-layer ping/pong (handled by websocketpp automatically),
    /// but also accepts JSON ping/pong — so the handler should still respond if one arrives.
    [[nodiscard]] static bool needs_json_ping(MarketType mt);

    // -----------------------------------------------------------------------
    // REST endpoint builders — common public/authenticated paths
    // -----------------------------------------------------------------------

    /// Path to list all trading instruments.
    ///
    /// Spot:    "/api/v4/spot/currency_pairs"
    /// Futures: "/api/v4/futures/usdt/contracts"
    [[nodiscard]] static std::string contracts_path(MarketType mt);

    /// Path to fetch ticker(s).
    ///
    /// Spot:    "/api/v4/spot/tickers"
    /// Futures: "/api/v4/futures/usdt/tickers"
    [[nodiscard]] static std::string tickers_path(MarketType mt);

    /// Path to fetch account balances.
    ///
    /// Spot:    "/api/v4/spot/accounts"
    /// Futures: "/api/v4/futures/usdt/accounts"
    [[nodiscard]] static std::string accounts_path(MarketType mt);
};

} // namespace pulse::exchange
