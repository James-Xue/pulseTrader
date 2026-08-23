#pragma once
// gate_kline_fetcher.hpp — Historical candle fetch from the Gate REST API (M29)
//
// Wraps GateRestClient::getSpotKlines / getFuturesKlines with:
//   - range pagination (Gate caps candles per request)
//   - pure parsing of the two response layouts (spot and futures differ!)
//   - dedup + sort so downstream consumers see a clean ascending series
//
// The parse/split helpers are static pure functions — unit-tested without a
// network. Only fetch() touches the wire.

#include "backtest/KlineSource.hpp"
#include "exchange/GateRestClient.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// GateKlineFetcher — IKlineSource backed by the exchange REST API
// ---------------------------------------------------------------------------
class GateKlineFetcher final : public IKlineSource
{
  public:
    /// Wrap a REST client. The client must outlive this fetcher.
    explicit GateKlineFetcher(exchange::GateRestClient &rest);

    /// Fetch candles in [from_ms, to_ms] by paginating backward from `to_ms`
    /// in chunks of `max_rows_per_request` × interval. Public endpoints.
    [[nodiscard]] Result<std::vector<market::Kline>> fetch(
        const std::string &symbol, MarketType market_type,
        std::int64_t from_ms, std::int64_t to_ms) override;

    [[nodiscard]] std::string description() const override;

    /// Map an interval in milliseconds to the Gate REST interval string.
    ///
    /// Supported: 1m / 5m / 15m / 30m / 1h / 4h / 1d. Unknown → PulseError.
    [[nodiscard]] static Result<std::string> intervalToString(std::int64_t interval_ms);

    /// Split [from_ms, to_ms] into sub-ranges each covering at most
    /// max_rows_per_request candles (ascending).
    ///
    /// Example: from=0, to=1000, interval=60s, max_rows=4 →
    ///   [(0, 240), (300, 540), (600, 840), (900, 1000)]
    [[nodiscard]] static std::vector<std::pair<std::int64_t, std::int64_t>> splitRange(
        std::int64_t from_ms, std::int64_t to_ms,
        std::int64_t interval_ms, std::int64_t max_rows_per_request);

    /// Parse a Gate candlesticks response into Kline structs.
    ///
    /// Response shapes differ by market (both probed live on 2026-08-23):
    ///   futures: array of OBJECTS { "t": sec, "o", "h", "l", "c", "v" }
    ///   spot:    array of ARRAYS  [ts, quote_vol, close, high, low, open,
    ///                              base_vol, closed]
    /// Timestamps are Unix seconds → converted to ms. Malformed rows are
    /// skipped with a WARN; the body not being an array is an error.
    [[nodiscard]] static Result<std::vector<market::Kline>> parseCandles(
        const nlohmann::json &body, MarketType market_type, std::int64_t interval_ms);

  private:
    exchange::GateRestClient &m_rest;
};

} // namespace pulse::backtest
