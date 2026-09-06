// iron_trader.cpp — IronTrader 铁律交易员 (rules baseline:
// docs/strategies/iron-trader.md — single source of truth)

#include "strategy/scalping/IronTrader.hpp"

#include "logging/Logger.hpp"
#include "strategy/NewsGate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace pulse::strategy
{

namespace
{

constexpr std::int64_t kMsPerDay = 86'400'000;
constexpr std::int64_t kMsPerHour = 3'600'000;

/// Deterministic EMA series over `closes`: SMA seed lands at index
/// period−1 (same convention as UnifiedScalper::computeEma); earlier
/// entries stay 0 (never read — warmup guarantees enough history).
std::vector<double> emaSeries(const std::vector<double> &closes, double period)
{
    std::vector<double> ema(closes.size(), 0.0);
    const auto p = static_cast<std::size_t>(period);
    if (closes.size() < p || 0 == p)
    {
        return ema;
    }

    const double k = 2.0 / (period + 1.0);
    double seed = 0.0;
    for (std::size_t i = 0; i < p; ++i)
    {
        seed += closes[i];
    }
    ema[p - 1] = seed / period;
    for (std::size_t i = p; i < closes.size(); ++i)
    {
        ema[i] = closes[i] * k + ema[i - 1] * (1.0 - k);
    }
    return ema;
}

/// True range of candle `cur` (uses `prev` for the gap terms).
double trueRange(const market::Kline &cur, const market::Kline &prev)
{
    const double hl = cur.high - cur.low;
    const double hpc = std::abs(cur.high - prev.close);
    const double lpc = std::abs(cur.low - prev.close);
    return std::max({ hl, hpc, lpc });
}

/// Median of a vector (upper-middle element — the regime gate's "typical
/// volatility" reading).
double medianTr(std::vector<double> trs)
{
    if (trs.empty())
    {
        return 0.0;
    }
    std::sort(trs.begin(), trs.end());
    return trs[trs.size() / 2];
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Identity / warmup
// ---------------------------------------------------------------------------

std::string IronTrader::className() const
{
    return "IronTrader";
}

std::string IronTrader::idPrefix() const
{
    return "iron_trader";
}

std::size_t IronTrader::klineNeeded() const
{
    // Trend EMA SMA seed + 60 spare bars (20-bar slope window + ATR/RSI
    // settling) — rules doc §5 暖机纪律.
    return static_cast<std::size_t>(param("it_trend_ema", 200.0)) + 60;
}

std::size_t IronTrader::warmupThreshold() const
{
    return klineNeeded();
}

bool IronTrader::cooldownEnabled() const
{
    // Cooldown is candle-based (it_cooldown_bars); wall-clock cooldown would
    // silently block exits under fast replay. See the header note.
    return false;
}

// ---------------------------------------------------------------------------
// Parameter-derived values
// ---------------------------------------------------------------------------

double IronTrader::param(const std::string &key, double fallback) const
{
    return customParam(key, fallback);
}

double IronTrader::sepGate(const Indicators &ind) const
{
    double gate = param("it_sep_gate", 0.25);
    if (ind.tight_session)
    {
        gate *= param("it_tight_gate_mult", 1.25);   // 时段分层 (G2)
    }
    if (m_after_streak_quota)
    {
        gate *= param("it_streak_gate_mult", 1.25);  // 连亏 2 后最强信号 (R6)
    }
    return gate;
}

double IronTrader::stopDistance(const Indicators &ind) const
{
    return param("it_sl_atr", 1.5) * ind.atr14;
}

// ---------------------------------------------------------------------------
// Indicators — deterministic fresh full-series recompute every candle
// ---------------------------------------------------------------------------

IronTrader::Indicators IronTrader::computeIndicators(
    const std::vector<market::Kline> &candles) const
{
    Indicators ind;
    const auto n = candles.size();
    if (n < 2)
    {
        return ind;
    }

    const double fast_p = param("it_fast_ema", 12.0);
    const double slow_p = param("it_slow_ema", 26.0);
    const double trend_p = param("it_trend_ema", 200.0);
    const double atr_p = param("it_atr_len", 14.0);
    const double rsi_p = param("it_rsi_len", 14.0);
    const auto bn = static_cast<std::size_t>(param("it_breakout_n", 20.0));

    std::vector<double> closes;
    closes.reserve(n);
    for (const auto &c : candles)
    {
        closes.push_back(c.close);
    }

    const auto ef = emaSeries(closes, fast_p);
    const auto es = emaSeries(closes, slow_p);
    const auto et = emaSeries(closes, trend_p);
    ind.ema_fast = ef[n - 1];
    ind.ema_slow = es[n - 1];
    ind.ema_trend = et[n - 1];
    ind.prev_close = closes[n - 2];
    ind.prev_ema_fast = ef[n - 2];
    ind.trend_prev20 = (n > 20) ? et[n - 1 - 20] : 0.0;

    // ATR + TR window over the last 20 true ranges.
    const std::size_t tr_window = 20;
    if (n < tr_window + 1)
    {
        return ind;
    }
    std::vector<double> trs;
    trs.reserve(tr_window);
    for (std::size_t i = n - tr_window; i < n; ++i)
    {
        trs.push_back(trueRange(candles[i], candles[i - 1]));
    }
    ind.med_tr20 = medianTr(trs);
    ind.max_tr20 = *std::max_element(trs.begin(), trs.end());

    const std::size_t atr_span = static_cast<std::size_t>(atr_p);
    if (n >= atr_span + 1)
    {
        double sum = 0.0;
        for (std::size_t i = n - atr_span; i < n; ++i)
        {
            sum += trueRange(candles[i], candles[i - 1]);
        }
        ind.atr14 = sum / static_cast<double>(atr_span);
    }

    if (ind.atr14 > 0.0 && ind.med_tr20 > 0.0)
    {
        ind.hot = ind.atr14 > param("it_regime_cap", 3.0) * ind.med_tr20;
        // 尖刺根:近 3 根任一根 TR > 3×medTR20 (rules §4.1 G3).
        for (std::size_t i = n - std::min<std::size_t>(3, tr_window); i < n; ++i)
        {
            if (trs[i - (n - tr_window)] > 3.0 * ind.med_tr20)
            {
                ind.spike_near = true;
                break;
            }
        }
        ind.sep = (ind.ema_fast - ind.ema_slow) / ind.atr14;
    }

    // RSI — simple average-gain/loss over the last `rsi_p` deltas.
    const auto rp = static_cast<std::size_t>(rsi_p);
    if (n >= rp + 1)
    {
        double gain = 0.0;
        double loss = 0.0;
        for (std::size_t i = n - rp; i < n; ++i)
        {
            const double d = closes[i] - closes[i - 1];
            if (d >= 0.0)
            {
                gain += d;
            }
            else
            {
                loss -= d;
            }
        }
        if (loss <= 0.0)
        {
            ind.rsi = 100.0;
        }
        else
        {
            ind.rsi = 100.0 - 100.0 / (1.0 + gain / loss);
        }
    }

    // Box = the last it_breakout_n CLOSED candles (current candle excluded).
    if (n > bn)
    {
        ind.box_high = candles[n - bn].high;
        ind.box_low = candles[n - bn].low;
        for (std::size_t i = n - bn + 1; i < n - 1; ++i)
        {
            ind.box_high = std::max(ind.box_high, candles[i].high);
            ind.box_low = std::min(ind.box_low, candles[i].low);
        }
    }

    // Session tiering: tight 20:00Z–06:00Z (rules §4.1 G2). open_time == 0
    // (synthetic test candles) counts as the main session.
    if (candles.back().open_time > 0)
    {
        const auto hour = (candles.back().open_time / kMsPerHour) % 24;
        ind.tight_session = (hour >= 20 || hour < 6);
    }
    return ind;
}

IronTrader::StructDir IronTrader::structDir(const Indicators &ind,
    double close) const
{
    if (close > ind.ema_trend && ind.ema_trend > ind.trend_prev20)
    {
        return StructDir::Bull;
    }
    if (close < ind.ema_trend && ind.ema_trend < ind.trend_prev20)
    {
        return StructDir::Bear;
    }
    return StructDir::None;
}

// ---------------------------------------------------------------------------
// Paper ledger (R0/R1/R6) — USD estimates; the account report is the
// authoritative PnL source (§9, marked [近似]).
// ---------------------------------------------------------------------------

double IronTrader::estimateNetUsd(bool is_long, double entry, double exit)
{
    const double qty = params().order_quantity.load(std::memory_order_acquire);
    const double quanto = param("it_quanto_est", 0.0001);
    const double fee_rate = param("it_fee_est", 0.0005);

    const double diff = is_long ? (exit - entry) : (entry - exit);
    const double gross = diff * qty * quanto;
    const double fees = fee_rate * (entry + exit) * qty * quanto;
    return gross - fees;
}

void IronTrader::settleTrade(bool is_long, double entry, double exit)
{
    const double net = estimateNetUsd(is_long, entry, exit);
    m_paper_equity_usd += net;
    m_day.day_realized_usd += net;
    ++m_day.trades_today;

    if (net < 0.0)
    {
        ++m_day.consec_losses;
    }
    else
    {
        m_day.consec_losses = 0;
    }

    const double day_pct = param("it_day_loss_pct", 5.0) / 100.0;
    if (m_day.day_realized_usd <= -day_pct * m_day.equity_ref_usd
        || m_day.consec_losses
            >= static_cast<int>(param("it_consec_loss", 3.0)))
    {
        m_day.day_stopped = true;   // R6 — 日亏红线压过一切
    }
    // 连亏 2 → 当日剩余额度仅 1 笔,且要求"最强信号" (R6)。
    if (2 == m_day.consec_losses && !m_day.day_stopped)
    {
        m_after_streak_quota = true;
    }
}

void IronTrader::rollDayIfNeeded(std::int64_t open_ms)
{
    const std::int64_t day_id = (open_ms > 0) ? (open_ms / kMsPerDay) : 0;
    if (m_equity_initialised && day_id == m_day.day_id)
    {
        return;
    }
    if (!m_equity_initialised)
    {
        m_paper_equity_usd = param("it_equity_ref_usd", 100.0);
        m_equity_initialised = true;
    }
    m_day = DayCounters{};
    m_day.day_id = day_id;
    m_day.equity_ref_usd = m_paper_equity_usd;   // R0: 日初权益快照
    m_after_streak_quota = false;
    m_quota_used = false;                        // R6 redemption quota is daily
}

// ---------------------------------------------------------------------------
// R1 / R6 pre-trade checklist (30 秒检查表, rules §3)
// ---------------------------------------------------------------------------

bool IronTrader::riskChecklistPasses(const Indicators &ind)
{
    if (m_day.day_stopped)
    {
        PULSE_LOG_INFO("iron_trader", "[{}] daily stop active (R6) — standing "
            "aside", id());
        return false;
    }
    if (m_day.trades_today >= static_cast<int>(param("it_max_trades_day", 6.0)))
    {
        PULSE_LOG_INFO("iron_trader", "[{}] day trade cap reached (R6) — "
            "standing aside", id());
        return false;
    }
    if (m_quota_used)
    {
        PULSE_LOG_INFO("iron_trader", "[{}] redemption trade already used "
            "(R6) — standing aside", id());
        return false;
    }

    // R1 — 单笔止损成本 ≤ it_risk_pct × 日初权益;超限 → 不发信号。
    const double qty = params().order_quantity.load(std::memory_order_acquire);
    const double quanto = param("it_quanto_est", 0.0001);
    const double stop_cost = stopDistance(ind) * qty * quanto;
    const double budget = (param("it_risk_pct", 2.0) / 100.0)
        * m_day.equity_ref_usd;
    if (stop_cost > budget)
    {
        PULSE_LOG_INFO("iron_trader", "[{}] R1 stop cost {:.6f} USD exceeds "
            "{:.4f} USD budget (2% of day-start equity) — no signal", id(),
            stop_cost, budget);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Position management — exit rules first (rules §4.3), then R7 protection
// ---------------------------------------------------------------------------

std::optional<EntryContext> IronTrader::managePosition(const market::Kline &cur,
    const Indicators &ind)
{
    const double close = cur.close;
    const bool is_long = (Phase::Long == m_phase);
    const double stop_dist = stopDistance(ind);

    // 1. Hard stop (R3) — close-based approximation (M33 收盘近似, §9).
    bool exit_now = ((is_long && close <= m_stop_price)
                     || (!is_long && close >= m_stop_price));
    std::string reason = "hard_stop";

    // 2. Structure reversal (G1 反向 → 提前离场).
    if (!exit_now)
    {
        const StructDir sd = structDir(ind, close);
        const bool flipped = is_long ? (StructDir::Bear == sd)
                                     : (StructDir::Bull == sd);
        if (flipped)
        {
            exit_now = true;
            reason = "structure_flip";
        }
    }
    // 3. Momentum reversal (动能反转 — sep crossed to the other sign).
    if (!exit_now)
    {
        const bool flipped = is_long ? (ind.sep < 0.0) : (ind.sep > 0.0);
        if (flipped)
        {
            exit_now = true;
            reason = "momentum_flip";
        }
    }
    // 4. Time stop (时间止损) — flat or small-profit holds are cut.
    if (!exit_now)
    {
        const double floating = is_long ? (close - m_entry_price)
                                        : (m_entry_price - close);
        if (m_bars_held >= static_cast<std::int64_t>(param("it_timeout_bars", 60.0))
            && floating < param("it_timeout_x_sl", 0.5) * stop_dist)
        {
            exit_now = true;
            reason = "time_stop";
        }
    }

    if (exit_now)
    {
        settleTrade(is_long, m_entry_price, close);

        EntryContext exit_ctx;
        exit_ctx.type = SignalType::Flat;   // 只平仓、不反手 (close-only channel)
        exit_ctx.price = close;
        exit_ctx.confidence = 0.0;
        char buf[96];
        std::snprintf(buf, sizeof(buf),
            "exit %s | held=%lld | close=%.2f", reason.c_str(),
            static_cast<long long>(m_bars_held), close);
        exit_ctx.reason = buf;
        exit_ctx.indicators = {
            { "state", is_long ? "long" : "short" },
            { "exit_reason", reason },
            { "close", close },
        };

        m_phase = Phase::Flat;
        m_flat_bars = 0;
        m_bars_held = 0;
        m_break_active = false;
        m_break_run = 0;
        return exit_ctx;
    }

    // No exit → R7 盈利保护:best close, breakeven once, then trail one way.
    const double floating = is_long ? (close - m_entry_price)
                                    : (m_entry_price - close);
    m_best_close = is_long ? std::max(m_best_close, close)
                           : std::min(m_best_close, close);
    if (!m_breakeven_done
        && floating >= param("it_be_x_sl", 1.0) * stop_dist)
    {
        m_stop_price = is_long ? std::max(m_stop_price, m_entry_price)
                               : std::min(m_stop_price, m_entry_price);
        m_breakeven_done = true;   // 保本 (R7)
    }
    if (m_breakeven_done)
    {
        const double trail = param("it_trail_atr", 1.0) * ind.atr14;
        m_stop_price = is_long
            ? std::max(m_stop_price, m_best_close - trail)
            : std::min(m_stop_price, m_best_close + trail);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 重大消息事件闸 (§4.6) — pre-event forced flatten; commit block mirrors the
// managePosition exit above with exit_reason = "news_blackout" (Flat channel)
// ---------------------------------------------------------------------------

std::optional<EntryContext> IronTrader::flattenForNews(const market::Kline &cur)
{
    const bool is_long = (Phase::Long == m_phase);
    const double close = cur.close;
    const std::string reason = "news_blackout";

    settleTrade(is_long, m_entry_price, close);

    EntryContext exit_ctx;
    exit_ctx.type = SignalType::Flat;   // 只平仓、不反手 (close-only channel)
    exit_ctx.price = close;
    exit_ctx.confidence = 0.0;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "exit %s | held=%lld | close=%.2f",
        reason.c_str(), static_cast<long long>(m_bars_held), close);
    exit_ctx.reason = buf;
    exit_ctx.indicators = {
        { "state", is_long ? "long" : "short" },
        { "exit_reason", reason },
        { "close", close },
    };

    PULSE_LOG_INFO("iron_trader", "[{}] news gate flatten (pre-event window) "
        "at open_time={} — closing {:.2f} ({})", id(), cur.open_time, close,
        is_long ? "long" : "short");

    m_phase = Phase::Flat;
    m_flat_bars = 0;
    m_bars_held = 0;
    m_break_active = false;
    m_break_run = 0;
    return exit_ctx;
}

bool IronTrader::newsFlattenDueNow(std::int64_t open_ms) const
{
    if (m_context.config.news_windows.empty())
    {
        return false;
    }
    return newsGateFlattenDue(m_context.config.news_windows, m_entry_open_ms,
        open_ms);
}

// ---------------------------------------------------------------------------
// Entry decisions — Flat phase only
// ---------------------------------------------------------------------------

std::optional<EntryContext> IronTrader::finishEntry(double close,
    const Indicators &ind, bool bull, double sep_gate_effective,
    const std::string &trigger, double sep_for_conf,
    std::int64_t entry_open_ms)
{
    // Re-run the checklist at the decision moment (R6 may have tripped while
    // a breakout was confirming).
    if (!riskChecklistPasses(ind))
    {
        m_break_active = false;
        m_break_run = 0;
        return std::nullopt;
    }

    const double stop_dist = stopDistance(ind);
    if (stop_dist <= 0.0)
    {
        return std::nullopt;
    }

    // Confidence: sep at/above the gate maps to ≥ 0.6 (the default engine
    // min_confidence), saturating at 1.0 two gates out.
    double confidence = 0.6
        + (std::abs(sep_for_conf) / sep_gate_effective - 1.0);
    confidence = std::clamp(confidence, 0.0, 1.0);

    m_phase = bull ? Phase::Long : Phase::Short;
    m_entry_open_ms = entry_open_ms;   // news gate pre-event flatten predicate
    m_entry_price = close;
    m_entry_atr = ind.atr14;
    m_stop_price = bull ? (close - stop_dist) : (close + stop_dist);
    m_best_close = close;
    m_breakeven_done = false;
    m_bars_held = 0;
    m_flat_bars = 0;
    m_break_active = false;
    m_break_run = 0;
    if (m_after_streak_quota)
    {
        // R6 redemption trade consumed — it is the day's last allowance.
        m_after_streak_quota = false;
        m_quota_used = true;
    }

    EntryContext entry;
    entry.type = bull ? SignalType::Buy : SignalType::Sell;
    entry.price = close;
    entry.atr = ind.atr14;
    entry.confidence = confidence;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s_%s | sep=%.3f | regime=%s",
        trigger.c_str(), bull ? "buy" : "sell", ind.sep,
        ind.hot ? "hot" : "normal");
    entry.reason = buf;
    entry.indicators = {
        { "trigger", trigger },
        { "close", close },
        { "ema_trend", ind.ema_trend },
        { "ema_fast", ind.ema_fast },
        { "ema_slow", ind.ema_slow },
        { "atr14", ind.atr14 },
        { "med_tr20", ind.med_tr20 },
        { "sep", ind.sep },
        { "regime", ind.hot ? "hot" : "normal" },
        { "spike_flag", ind.spike_near },
        { "rsi14", ind.rsi },
        { "box_high", ind.box_high },
        { "box_low", ind.box_low },
        { "state", bull ? "long" : "short" },
        { "tight_session", ind.tight_session },
    };
    return entry;
}

std::optional<EntryContext> IronTrader::evaluateEntrySetup(
    const std::vector<market::Kline> &candles, const Indicators &ind,
    StructDir struct_dir, double close)
{
    // Candle-based cooldown after the last exit (tight session adds bars).
    std::int64_t cooldown
        = static_cast<std::int64_t>(param("it_cooldown_bars", 3.0));
    if (ind.tight_session)
    {
        cooldown += static_cast<std::int64_t>(param("it_tight_cooldown_add", 1.0));
    }
    if (m_flat_bars < cooldown)
    {
        return std::nullopt;
    }
    if (ind.atr14 <= 0.0)
    {
        return std::nullopt;
    }

    const double sep_gate = sepGate(ind);
    const bool bull = (StructDir::Bull == struct_dir);

    // ---- T-B breakout confirmation machine (a machine may already run) ----
    if (m_break_active)
    {
        // Structure reversed while confirming → the setup is dead.
        if (StructDir::None == struct_dir || bull != m_break_bull)
        {
            m_break_active = false;
            m_break_run = 0;
            return std::nullopt;
        }
        const bool beyond = bull ? (close > m_break_level)
                                 : (close < m_break_level);
        if (!beyond)
        {
            // 假破位 — 触发作废,锚点随箱体每根重算自然复位。
            m_break_active = false;
            m_break_run = 0;
            return std::nullopt;
        }
        ++m_break_run;
        if (m_break_run > 4)
        {
            m_break_active = false;   // stale — never confirmed
            m_break_run = 0;
            return std::nullopt;
        }

        // Entry needs ≥ 2 consecutive closes beyond; a spike root anywhere in
        // the window demands one extra NORMAL bar outside (回合 446 复证) —
        // !ind.spike_near supplies exactly that.
        if (m_break_run >= 2 && !ind.spike_near && !ind.hot
            && std::abs(ind.sep) >= sep_gate
            && ind.max_tr20 <= param("it_spike_skip_mult", 2.0)
                * stopDistance(ind))
        {
            // RSI 对冲过滤 (M22): 超买不追多破位 / 超卖不追空破位 (T-B only).
            const bool rsi_blocks = bull ? (ind.rsi > param("it_rsi_ob", 70.0))
                                         : (ind.rsi < param("it_rsi_os", 30.0));
            if (rsi_blocks)
            {
                m_break_active = false;
                m_break_run = 0;
                return std::nullopt;
            }
            return finishEntry(close, ind, bull, sep_gate, "box_breakout",
                m_break_sep, candles.back().open_time);
        }
        return std::nullopt;
    }

    // New setups require structure + momentum + volatility gates (G1–G3).
    if (StructDir::None == struct_dir || ind.hot || ind.spike_near
        || std::abs(ind.sep) < sep_gate
        || (bull ? ind.sep < 0.0 : ind.sep > 0.0))
    {
        return std::nullopt;
    }
    if (ind.max_tr20 > param("it_spike_skip_mult", 2.0) * stopDistance(ind))
    {
        PULSE_LOG_INFO("iron_trader", "[{}] maxTR20 {:.2f} > {:.2f} × stop "
            "distance — skipping entry (R5)", id(), ind.max_tr20,
            param("it_spike_skip_mult", 2.0));
        return std::nullopt;
    }

    // New T-B candidate: close breaks the N-bar box. The break bar itself
    // counts as the first close beyond (等下一根收盘确认 → run ≥ 2 fires).
    const double level = bull ? ind.box_high : ind.box_low;
    const bool broke = bull ? (close > level) : (close < level);
    if (broke)
    {
        m_break_active = true;
        m_break_bull = bull;
        m_break_level = level;
        m_break_sep = ind.sep;
        m_break_run = 1;
        return std::nullopt;
    }

    // T-A 拉回:上根收 < EMA_fast(多向),本根收复 EMA_fast → 入场 (首选)。
    const bool pullback = bull
        ? (ind.prev_close < ind.prev_ema_fast && close > ind.ema_fast)
        : (ind.prev_close > ind.prev_ema_fast && close < ind.ema_fast);
    if (pullback)
    {
        return finishEntry(close, ind, bull, sep_gate, "pullback_resume",
            ind.sep, candles.back().open_time);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// evaluateEntry — per-candle state machine entry point
// ---------------------------------------------------------------------------

std::optional<EntryContext> IronTrader::evaluateEntry(
    const std::vector<market::Kline> &candles)
{
    if (candles.empty())
    {
        return std::nullopt;
    }
    rollDayIfNeeded(candles.back().open_time);
    const Indicators ind = computeIndicators(candles);
    const double close = candles.back().close;

    // 重大消息事件闸 (§4.6): a position opened before a listed event must be
    // flat from the first candle with open_time ≥ T−X — the gate reason wins
    // over any coincident exit reason (either way the position closes and
    // settleTrade books the same realized PnL).
    const bool news_flatten_due = newsFlattenDueNow(candles.back().open_time);

    if (Phase::Flat != m_phase)
    {
        ++m_bars_held;
        if (news_flatten_due)
        {
            return flattenForNews(candles.back());
        }
        return managePosition(candles.back(), ind);   // 有仓 → 只做管理 (R2)
    }

    ++m_flat_bars;
    if (newsGateBlocked(candles.back().open_time))
    {
        // Blackout window: no entries; the T-B machine is stood down so an
        // event-spike bar can never serve as a confirmation bar after resume.
        m_break_active = false;
        m_break_run = 0;
        return std::nullopt;
    }
    return evaluateEntrySetup(candles, ind, structDir(ind, close), close);
}

} // namespace pulse::strategy
