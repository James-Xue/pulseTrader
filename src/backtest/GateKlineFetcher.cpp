// gate_kline_fetcher.cpp — Historical candle fetch from the Gate REST API (M29)

#include "backtest/GateKlineFetcher.hpp"

#include "logging/Logger.hpp"

#include <algorithm>

namespace pulse::backtest
{

namespace
{

/// Parse one numeric cell that may arrive as number or string.
std::optional<double> cellToDouble(const nlohmann::json &cell)
{
    if (cell.is_string())
    {
        try
        {
            return std::stod(cell.get<std::string>());
        }
        catch (const std::exception &)
        {
            return std::nullopt;
        }
    }
    if (cell.is_number())
    {
        return cell.get<double>();
    }
    return std::nullopt;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GateKlineFetcher::GateKlineFetcher(exchange::GateRestClient &rest)
    : m_rest{ rest }
{
}

std::string GateKlineFetcher::description() const
{
    return "gate-api";
}

// ---------------------------------------------------------------------------
// intervalToString — Gate REST interval identifier
// ---------------------------------------------------------------------------

Result<std::string> GateKlineFetcher::intervalToString(std::int64_t interval_ms)
{
    switch (interval_ms)
    {
        case 60'000: return std::string{ "1m" };
        case 300'000: return std::string{ "5m" };
        case 900'000: return std::string{ "15m" };
        case 1'800'000: return std::string{ "30m" };
        case 3'600'000: return std::string{ "1h" };
        case 14'400'000: return std::string{ "4h" };
        case 86'400'000: return std::string{ "1d" };
        default:
            return PulseError{ ErrorCode::BacktestRangeInvalid,
                "Unsupported kline interval for Gate API: " + std::to_string(interval_ms)
                    + " ms (supported: 1m/5m/15m/30m/1h/4h/1d)" };
    }
}

// ---------------------------------------------------------------------------
// splitRange — chunk a window into per-request sub-ranges
// ---------------------------------------------------------------------------

std::vector<std::pair<std::int64_t, std::int64_t>> GateKlineFetcher::splitRange(
    std::int64_t from_ms, std::int64_t to_ms,
    std::int64_t interval_ms, std::int64_t max_rows_per_request)
{
    std::vector<std::pair<std::int64_t, std::int64_t>> chunks;
    if (to_ms <= from_ms || interval_ms <= 0 || max_rows_per_request <= 0)
    {
        return chunks;
    }

    const std::int64_t step_ms = interval_ms * max_rows_per_request;
    for (std::int64_t start = from_ms; start <= to_ms; start += step_ms)
    {
        const std::int64_t end = std::min(start + step_ms - 1, to_ms);
        chunks.emplace_back(start, end);
    }
    return chunks;
}

// ---------------------------------------------------------------------------
// parseCandles — Gate candlesticks response → Kline vector
//
// Response shapes differ by market (both probed live on 2026-08-23):
//   futures: array of OBJECTS { "t": sec, "o", "h", "l", "c", "v", ... }
//   spot:    array of ARRAYS  [ts, quote_vol, close, high, low, open,
//                              base_vol, closed]
// The object/array distinction is self-describing, but volume semantics
// follow the market: futures "v" is base volume, spot index 6 is base volume.
// ---------------------------------------------------------------------------

Result<std::vector<market::Kline>> GateKlineFetcher::parseCandles(
    const nlohmann::json &body, MarketType market_type, std::int64_t interval_ms)
{
    if (!body.is_array())
    {
        return PulseError{ ErrorCode::BacktestApiFetchFailed,
            "Gate candlesticks response is not an array" };
    }

    std::vector<market::Kline> candles;
    int skipped = 0;

    for (const auto &row : body)
    {
        market::Kline kline;
        std::optional<double> open, high, low, close, volume;
        bool parsed = false;

        if (MarketType::Futures == market_type && row.is_object())
        {
            // Futures layout: {"t": <sec>, "o", "h", "l", "c", "v"}.
            const auto &t = row.value("t", nlohmann::json());
            if (t.is_string())
            {
                try
                {
                    kline.open_time = std::stoll(t.get<std::string>()) * 1000;
                    parsed = true;
                }
                catch (const std::exception &)
                {
                }
            }
            else if (t.is_number())
            {
                kline.open_time = static_cast<std::int64_t>(t.get<double>()) * 1000;
                parsed = true;
            }
            if (parsed)
            {
                open = cellToDouble(row.value("o", nlohmann::json()));
                high = cellToDouble(row.value("h", nlohmann::json()));
                low = cellToDouble(row.value("l", nlohmann::json()));
                close = cellToDouble(row.value("c", nlohmann::json()));
                volume = cellToDouble(row.value("v", nlohmann::json()));
            }
        }
        else if (MarketType::Spot == market_type && row.is_array() && row.size() >= 7)
        {
            // Spot layout: [ts, quote_vol, close, high, low, open, base_vol, closed].
            const nlohmann::json &ts_cell = row[0];
            if (ts_cell.is_string())
            {
                try
                {
                    kline.open_time = std::stoll(ts_cell.get<std::string>()) * 1000;
                    parsed = true;
                }
                catch (const std::exception &)
                {
                }
            }
            else if (ts_cell.is_number())
            {
                kline.open_time = static_cast<std::int64_t>(ts_cell.get<double>()) * 1000;
                parsed = true;
            }
            if (parsed)
            {
                close = cellToDouble(row[2]);
                high = cellToDouble(row[3]);
                low = cellToDouble(row[4]);
                open = cellToDouble(row[5]);
                volume = cellToDouble(row[6]);
            }
        }
        else
        {
            ++skipped;
            continue;
        }

        if (!parsed || !close || !high || !low || !open || !volume)
        {
            ++skipped;
            continue;
        }

        kline.close_time = kline.open_time + interval_ms;
        kline.open = open.value();
        kline.high = high.value();
        kline.low = low.value();
        kline.close = close.value();
        kline.volume = volume.value();
        kline.closed = true;

        candles.push_back(kline);
    }

    if (0 < skipped)
    {
        PULSE_LOG_WARN("backtest", "parseCandles skipped {} malformed rows", skipped);
    }

    std::sort(candles.begin(), candles.end(),
        [](const market::Kline &a, const market::Kline &b)
        {
            return a.open_time < b.open_time;
        });
    return candles;
}

// ---------------------------------------------------------------------------
// fetch — paginated range fetch
// ---------------------------------------------------------------------------

Result<std::vector<market::Kline>> GateKlineFetcher::fetch(
    const std::string &symbol, MarketType market_type,
    std::int64_t from_ms, std::int64_t to_ms)
{
    constexpr std::int64_t kMaxRowsPerRequest = 1000;
    constexpr std::int64_t kIntervalMs = 60'000; // MVP fetches 1m bars only.

    if (MarketType::Cfd == market_type)
    {
        return PulseError{ ErrorCode::BacktestRangeInvalid,
            "GateKlineFetcher does not support CFD (TradFi klines have no "
            "time-range API)" };
    }

    auto interval_str = intervalToString(kIntervalMs);
    if (!ok(interval_str))
    {
        return error(interval_str);
    }

    const auto chunks = splitRange(from_ms, to_ms, kIntervalMs, kMaxRowsPerRequest);

    std::vector<market::Kline> all;
    for (const auto &[chunk_from, chunk_to] : chunks)
    {
        // limit must be omitted when from/to are present (Gate rejects the
        // combination with HTTP 400 — probed 2026-08-23).
        Result<nlohmann::json> resp = (MarketType::Futures == market_type)
            ? m_rest.getFuturesKlines(symbol, value(interval_str),
                  chunk_from / 1000, chunk_to / 1000, 0)
            : m_rest.getSpotKlines(symbol, value(interval_str),
                  chunk_from / 1000, chunk_to / 1000, 0);

        if (!ok(resp))
        {
            return PulseError{ ErrorCode::BacktestApiFetchFailed,
                "Gate kline fetch failed for " + symbol + " [" +
                std::to_string(chunk_from) + ", " + std::to_string(chunk_to)
                + "]: " + error(resp).message };
        }

        auto parsed = parseCandles(value(resp), market_type, kIntervalMs);
        if (!ok(parsed))
        {
            return error(parsed);
        }
        auto chunk_candles = value(parsed);

        // Keep only candles inside the chunk window (API may over-deliver at
        // the edges) and merge.
        for (auto &c : chunk_candles)
        {
            if (chunk_from <= c.open_time && c.open_time <= chunk_to)
            {
                all.push_back(std::move(c));
            }
        }
    }

    std::sort(all.begin(), all.end(),
        [](const market::Kline &a, const market::Kline &b)
        {
            return a.open_time < b.open_time;
        });
    return all;
}

} // namespace pulse::backtest
