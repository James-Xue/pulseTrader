// replay_driver.cpp — Historical candle replay through a real strategy (M29)

#include "backtest/ReplayDriver.hpp"

namespace pulse::backtest
{

// ---------------------------------------------------------------------------
// Construction — offline feed + strategy from options
// ---------------------------------------------------------------------------

ReplayDriver::ReplayDriver(const BacktestOptions &opts,
                           strategy::StrategyRegistry &registry)
    : m_opts{ opts }
    , m_rest{ ExchangeConfig{}, opts.market_type }
    , m_feed{ nullptr, m_rest, opts.market_type }
{
    // Build the instance context the way main.cpp does: the strategy reads
    // its feed through the context; executor/risk stay null (signals only —
    // fills are simulated by BacktestAccount, never sent anywhere).
    strategy::StrategyContext ctx;
    ctx.config.name = opts.strategy_name;
    ctx.config.symbol = opts.symbol;
    ctx.config.market_type = opts.market_type;
    ctx.config.order_quantity = (opts.order_quantity > 0.0)
        ? opts.order_quantity : 0.001;
    ctx.config.min_confidence = opts.min_confidence;
    ctx.market_feed = &m_feed;

    m_strategy = registry.create(opts.strategy_name, ctx);

    // Seed live params the way main.cpp:888-906 does. Cooldown defaults to 0
    // (wall-clock cooldown is meaningless under fast replay — documented
    // simplification); min_confidence and order_quantity honor the CLI.
    m_strategy->params().min_confidence.store(opts.min_confidence,
                                              std::memory_order_release);
    m_strategy->params().order_quantity.store(ctx.config.order_quantity,
                                              std::memory_order_release);
    m_strategy->params().cooldown_seconds.store(opts.cooldown_seconds,
                                                std::memory_order_release);
    m_strategy->params().auto_trade.store(0.0, std::memory_order_release);

    // Wire the signal callback: collect non-Flat signals and forward every
    // signal to the account. The candle open time and feed index ride in via
    // loop-local members (callbacks fire synchronously inside onKline).
    m_strategy->setSignalCallback(
        [this](const strategy::TradingSignal &sig)
        {
            if (strategy::SignalType::Flat != sig.type)
            {
                BacktestSignal bs;
                bs.candle_open_ms = m_currentCandleOpenMs;
                bs.type = sig.type;
                bs.confidence = sig.confidence;
                bs.price = sig.price;
                bs.reason = sig.reason;
                bs.indicators = sig.indicators;
                if (-1 == m_firstSignalIndex)
                {
                    m_firstSignalIndex = m_currentCandleIndex;
                }
                m_signals.push_back(bs);
            }
            m_account->onSignal(sig, m_currentCandleOpenMs);
        });
}

// ---------------------------------------------------------------------------
// run — replay loop
// ---------------------------------------------------------------------------

Result<ReplayResult> ReplayDriver::run(
    const std::vector<market::Kline> &candles, BacktestAccount &account)
{
    m_account = &account;
    m_signals.clear();
    m_firstSignalIndex = -1;

    ReplayResult result;
    result.candles_fed = candles.size();

    auto &buffer = m_feed.getKlineBuffer(m_opts.symbol);

    for (std::size_t i = 0; i < candles.size(); ++i)
    {
        const auto &candle = candles[i];
        buffer.push(candle);

        m_currentCandleIndex = static_cast<std::int64_t>(i);
        m_currentCandleOpenMs = candle.open_time;
        m_strategy->onKline(candle);
    }

    // Warmup: candles fed before the first signal. When the first evaluated
    // candle already signals (the common case) this equals the strategy's
    // true warmup window — e.g. 200 for EmaResonance's 201-bar warmup.
    result.warmup_candles = (m_firstSignalIndex >= 0)
        ? static_cast<std::size_t>(m_firstSignalIndex)
        : candles.size();

    // Evaluations: candles fed at/after the warmup point (the strategy
    // template evaluates every candle once the buffer holds enough).
    result.evaluations = (result.warmup_candles <= candles.size())
        ? candles.size() - result.warmup_candles
        : 0;

    result.signals = std::move(m_signals);
    m_account = nullptr;
    return result;
}

} // namespace pulse::backtest
