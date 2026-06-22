// supertrend_scalper.cpp — SuperTrend indicator strategy (Layer 6 Strategy Engine)

#include "strategy/scalping/supertrend_scalper.hpp"

#include "logging/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pulse::strategy
{

SuperTrendScalper::SuperTrendScalper(const StrategyContext &context)
{
    context_ = context;
}

std::string SuperTrendScalper::name() const
{
    return "SuperTrendScalper";
}

std::string SuperTrendScalper::id() const
{
    return "supertrend_scalper_" + context_.config.symbol;
}

StrategyParams &SuperTrendScalper::params()
{
    return params_;
}

void SuperTrendScalper::on_tick(const market::Ticker & /*ticker*/)
{
    // This strategy is kline-driven; tick updates are ignored for trading.
    // However, we use on_tick() to detect "no kline data at all" (e.g. WS not connected).
    auto *feed = context_.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->get_kline_buffer(context_.config.symbol).snapshot(1);
    if (candles.empty())
    {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (now_ms - last_no_data_log_ms_ >= 30'000)
        {
            PULSE_LOG_INFO("strategy",
                "[{}] Waiting for kline data (WS may not be connected yet)", id());
            last_no_data_log_ms_ = now_ms;
        }
    }
}

void SuperTrendScalper::on_orderbook(const market::OrderBook & /*book*/)
{
    // This strategy is kline-driven; orderbook updates are ignored.
}

void SuperTrendScalper::on_kline(const market::Kline & /*kline*/)
{
    // 1. Read hot-reloadable parameters (lock-free atomic loads).
    const auto period = static_cast<std::size_t>(
        params_.supertrend_period.load(std::memory_order_acquire));
    const double multiplier = params_.supertrend_multiplier.load(std::memory_order_acquire);
    const auto cooldown_sec = params_.cooldown_seconds.load(std::memory_order_acquire);

    // 2. Fetch enough candles to compute ATR and SuperTrend.
    //    Need period + 1 candles: period for ATR, +1 for the "previous close" of the first TR.
    const auto needed = period + 1;
    auto *feed = context_.market_feed;
    if (nullptr == feed)
    {
        return;
    }

    auto candles = feed->get_kline_buffer(context_.config.symbol).snapshot(needed);
    if (candles.size() < needed)
    {
        // Not enough data yet — log warmup progress every 30 seconds.
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                .count();
        if (now_ms - last_warmup_log_ms_ >= 30'000)
        {
            PULSE_LOG_INFO("strategy",
                "[{}] Warming up: {}/{} candles accumulated (need ~{} min of kline data)",
                id(), candles.size(), needed, needed);
            last_warmup_log_ms_ = now_ms;
        }
        return;
    }

    // 3. Compute ATR.
    const double atr = compute_atr(candles, period);
    if (0.0 >= atr)
    {
        return; // ATR is zero — market is completely flat, skip.
    }

    // 4. Compute basic bands for the latest candle.
    const auto &latest = candles.back();
    const double midpoint = (latest.high + latest.low) / 2.0;
    const double basic_upper = midpoint + multiplier * atr;
    const double basic_lower = midpoint - multiplier * atr;

    // 5. Compute final bands (tightening logic).
    //    Upper band: take the lower of basic_upper and prev_upper (tighten upward moves).
    //    Reset if previous close broke above the previous upper band.
    double final_upper = basic_upper;
    if (has_prev_)
    {
        if (basic_upper < prev_upper_band_ || prev_close_ > prev_upper_band_)
        {
            final_upper = basic_upper;
        }
        else
        {
            final_upper = prev_upper_band_;
        }
    }

    //    Lower band: take the higher of basic_lower and prev_lower (tighten downward moves).
    //    Reset if previous close broke below the previous lower band.
    double final_lower = basic_lower;
    if (has_prev_)
    {
        if (basic_lower > prev_lower_band_ || prev_close_ < prev_lower_band_)
        {
            final_lower = basic_lower;
        }
        else
        {
            final_lower = prev_lower_band_;
        }
    }

    // 6. Determine current SuperTrend value and trend direction.
    bool current_bullish = is_bullish_;
    double current_supertrend = 0.0;

    if (has_prev_)
    {
        // If we were bullish and close stays above final_lower → stay bullish.
        if (is_bullish_ && latest.close >= final_lower)
        {
            current_bullish = true;
            current_supertrend = final_lower;
        }
        // If we were bearish and close stays below final_upper → stay bearish.
        else if (!is_bullish_ && latest.close <= final_upper)
        {
            current_bullish = false;
            current_supertrend = final_upper;
        }
        // Otherwise: trend flipped.
        else if (is_bullish_ && latest.close < final_lower)
        {
            current_bullish = false;
            current_supertrend = final_upper;
        }
        else // !is_bullish_ && close > final_upper
        {
            current_bullish = true;
            current_supertrend = final_lower;
        }
    }
    else
    {
        // First computation: infer trend from price position relative to midpoint.
        current_bullish = (latest.close >= midpoint);
        current_supertrend = current_bullish ? final_lower : final_upper;
    }

    // 7. Detect trend flip and emit signal.
    if (has_prev_)
    {
        const bool flipped_bullish = !is_bullish_ && current_bullish;
        const bool flipped_bearish = is_bullish_ && !current_bullish;

        if (flipped_bullish || flipped_bearish)
        {
            // Enforce cooldown.
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            const auto cooldown_ms = static_cast<std::int64_t>(cooldown_sec * 1000.0);
            if (now_ms - last_signal_time_ms_ >= cooldown_ms)
            {
                // 8. Compute confidence: distance from price to SuperTrend, normalized by ATR.
                double confidence = std::abs(latest.close - current_supertrend) / atr;
                confidence = std::clamp(confidence, 0.0, 1.0);

                // 9. Build and emit signal.
                TradingSignal signal;
                signal.type = flipped_bullish ? SignalType::Buy : SignalType::Sell;
                signal.symbol = context_.config.symbol;
                signal.confidence = confidence;
                signal.price = latest.close;
                signal.strategy_id = id();
                signal.timestamp = now();
                signal.reason = flipped_bullish
                    ? "SuperTrend flipped bullish (price crossed above band)"
                    : "SuperTrend flipped bearish (price crossed below band)";

                PULSE_LOG_INFO("strategy", "[{}] {} signal: confidence={:.4f}, price={:.2f}, atr={:.2f}",
                    id(), signal.reason, confidence, signal.price, atr);

                emit_signal(signal);
                last_signal_time_ms_ = now_ms;
            }
        }
    }

    // 10. Store state for next candle.
    prev_upper_band_ = final_upper;
    prev_lower_band_ = final_lower;
    prev_close_ = latest.close;
    prev_supertrend_ = current_supertrend;
    is_bullish_ = current_bullish;
    has_prev_ = true;
}

double SuperTrendScalper::compute_atr(const std::vector<market::Kline> &candles,
    std::size_t period) const
{
    // Need at least period + 1 candles to compute `period` true ranges.
    if (candles.size() < period + 1)
    {
        return 0.0;
    }

    // Compute True Range for the last `period` candles.
    // TR = max(high - low, |high - prev_close|, |low - prev_close|)
    double sum_tr = 0.0;
    const auto start = candles.size() - period;
    for (std::size_t i = start; i < candles.size(); ++i)
    {
        const double hl = candles[i].high - candles[i].low;
        const double hpc = std::abs(candles[i].high - candles[i - 1].close);
        const double lpc = std::abs(candles[i].low - candles[i - 1].close);
        sum_tr += std::max({ hl, hpc, lpc });
    }

    return sum_tr / static_cast<double>(period);
}

} // namespace pulse::strategy
