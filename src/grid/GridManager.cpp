// grid_manager.cpp — engine-native grid execution service (M27)
//
// See GridManager.hpp for the full rule summary. The exchange is the source
// of truth for open orders (IGridGateway::openFuturesOrders); the engine
// tracker loses state on restart, the exchange does not.

#include "grid/GridManager.hpp"

#include "logging/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

namespace pulse::grid
{

namespace
{

constexpr std::int64_t kSlowEvery = 250;    ///< ticks between slow passes (~50s @200ms).
constexpr std::int64_t kMidEvery = 5;       ///< ticks between mid passes (~1s).
constexpr std::int64_t kSignalFreshMs = 120000; ///< eth_scalper state freshness (≤120s).
constexpr double kEthQuanto = 0.01;         ///< 1 ETH_USDT contract = 0.01 ETH.

/// "eth-grid-sell-<price>" / "eth-grid-tp-<fill>" client id prefixes.
std::string sellId(double price)
{
    std::ostringstream os;
    os << "eth-grid-sell-" << static_cast<std::int64_t>(std::round(price * 100.0)) / 100.0;
    return os.str();
}

std::string tpId(double fill_price)
{
    std::ostringstream os;
    os << "eth-grid-tp-" << static_cast<std::int64_t>(std::round(fill_price * 100.0)) / 100.0;
    return os.str();
}

bool startsWith(const std::string &s, const std::string &prefix)
{
    return s.rfind(prefix, 0) == 0;
}

/// Round to the nearest multiple of step (grid price quantization).
double roundToStep(double value, double step)
{
    return std::round(value / step) * step;
}

/// Wall-clock epoch ms (tests pass their own timestamps to tick()).
std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

GridManager::GridManager(const GridConfig &cfg, IGridGateway &gw,
                         strategy::SignalBoard &board,
                         market::MarketFeed *futures_feed,
                         std::mutex &rest_mutex,
                         std::filesystem::path state_dir)
    : m_cfg{ cfg }
    , m_gw{ gw }
    , m_board{ board }
    , m_feed{ futures_feed }
    , m_restMutex{ rest_mutex }
    , m_stateFile{ std::move(state_dir) / cfg.state_file }
    , m_symbol{ cfg.symbol }
{
}

Result<GridSnapshot> GridManager::start(const nlohmann::json &overrides)
{
    std::lock_guard lock{ m_mutex };

    if (GridPhase::Disabled != m_phase)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "grid already running (phase="
                               + std::to_string(static_cast<int>(m_phase)) + ")" };
    }

    // Pre-check: a non-grid position on the symbol means the grid share is
    // merged with user exposure — protection-line closes could touch it.
    refreshUserPositionWarn();
    if (m_userPositionWarn && !m_cfg.force)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "grid start refused: non-grid positions exist on "
                               + m_symbol
                               + " (user share would merge with the grid); "
                                 "set grid.force = true to proceed" };
    }

    // Overrides (grid_start --levels/--qty/--step/--anchor) — formal support
    // lands with the control-plane wiring (PR-4); here they are logged only.
    if (!overrides.is_null() && !overrides.empty())
    {
        PULSE_LOG_INFO("grid", "{}: start overrides noted ({} keys)",
                       m_symbol, overrides.size());
    }

    const auto now_ms = nowMs();
    recomputeLevels(now_ms); // sets m_anchor / m_step from current mid
    m_phase = GridPhase::Running;
    setLastAction(GridAction::HangLevels, now_ms);
    m_stateDirty = true;
    (void)saveState();
    return statusLocked(); // lock already held
}

GridSnapshot GridManager::status() const
{
    std::lock_guard lock{ m_mutex };
    return statusLocked();
}

GridSnapshot GridManager::statusLocked() const
{
    GridSnapshot snap;
    snap.phase = m_phase;
    snap.symbol = m_symbol;
    snap.anchor = m_anchor;
    snap.step = m_step;
    snap.top = m_anchor + m_cfg.levels * m_step;
    snap.realized_pnl_today = m_realizedToday;
    snap.trend_gate = m_trendGate;
    snap.spike_frozen = nowMs() < m_spikeUntil;
    snap.spike_until_ms = m_spikeUntil;
    snap.daily_loss_frozen = m_dailyFrozen;
    snap.reanchor_until_ms = m_reanchorUntil;
    snap.user_position_warn = m_userPositionWarn;
    snap.last_action = m_lastActionName;
    snap.last_action_ms = m_lastActionMs;
    snap.levels = m_levels;
    for (const auto &lv : m_levels)
    {
        snap.levels_filled += lv.filled;
    }

    // Floating loss of the grid share (filled levels only, short side).
    const double mid = computeMid();
    for (const auto &lv : m_levels)
    {
        if (lv.filled > 0 && lv.fill_price > 0.0)
        {
            snap.unrealized_pnl += (lv.fill_price - mid) * lv.filled * kEthQuanto;
        }
    }
    return snap;
}

Result<GridSnapshot> GridManager::pause()
{
    std::lock_guard lock{ m_mutex };
    if (GridPhase::Running != m_phase && GridPhase::Protecting != m_phase)
    {
        return PulseError{ ErrorCode::ControlInvalidRequest,
                           "grid not running (phase="
                               + std::to_string(static_cast<int>(m_phase)) + ")" };
    }
    m_phase = GridPhase::Paused;
    setLastAction(GridAction::None, nowMs());
    return statusLocked();
}

Result<GridSnapshot> GridManager::stop()
{
    std::lock_guard lock{ m_mutex };
    cancelAllGridOrders();
    m_levels.clear();
    m_levelByOrderId.clear();
    m_phase = GridPhase::Disabled;
    setLastAction(GridAction::None, nowMs());
    m_stateDirty = true;
    (void)saveState();
    return statusLocked();
}

void GridManager::tick(std::int64_t now_ms)
{
    // Cheap path: disabled grids do nothing (control-plane lock already
    // guards the phase read; a concurrent start will take the lock below).
    {
        std::lock_guard lock{ m_mutex };
        if (GridPhase::Disabled == m_phase)
        {
            return;
        }
    }

    std::lock_guard lock{ m_mutex };
    m_lastTickMs = now_ms;

    tickFast(now_ms);
    if (++m_tickCounter % kMidEvery == 0)
    {
        tickMid(now_ms);
    }
    if (m_tickCounter % kSlowEvery == 0)
    {
        tickSlow(now_ms);
    }

    if (m_stateDirty)
    {
        m_stateDirty = false;
        (void)saveState();
    }
}

void GridManager::onDirectionSwitched()
{
    // Futures orders may have been swept by cancelAllOpenOrders — the slow
    // pass will re-hang whatever the trend gate allows. Set the external
    // cancel flag so the next reconcile treats disappearances as CANCELS,
    // not fills (a swept sell order must not increment the level ledger).
    std::lock_guard lock{ m_mutex };
    m_externalCancelPending = true;
    PULSE_LOG_INFO("grid",
        "{}: direction switched — grid orders may have been swept; "
        "next reconcile will re-hang (trend-gated)", m_symbol);
}

// ---------------------------------------------------------------------------
// Tick layers
// ---------------------------------------------------------------------------

void GridManager::tickFast(std::int64_t now_ms)
{
    // Daily boundary: Beijing 08:00 == UTC 00:00 (UTC+8). When the configured
    // reset hour differs, shift the window so the boundary still lands there.
    const double shift_h = static_cast<double>(m_cfg.daily_reset_hour - 8);
    const std::int64_t now_sec = now_ms / 1000;
    const std::int64_t day_key = now_sec + static_cast<std::int64_t>(shift_h * 3600.0);
    const std::int64_t day_start = (day_key / 86400) * 86400;
    if (day_start != m_dayStartSec)
    {
        m_realizedToday = 0.0;
        m_dailyFrozen = false;
        m_dayStartSec = day_start;
        m_stateDirty = true;
        PULSE_LOG_INFO("grid", "{}: new trading day — realized window reset", m_symbol);
    }

    markSpikeIfNeeded(now_ms);
}

void GridManager::tickMid(std::int64_t now_ms)
{
    // Trend gate refresh (SignalBoard eth_scalper state; "stale" when
    // missing/expired — new levels stay frozen until fresh data arrives).
    m_trendGate = readTrendGate(now_ms);

    // Freeze expiry.
    if (m_spikeUntil != 0 && now_ms >= m_spikeUntil)
    {
        m_spikeUntil = 0;
        m_stateDirty = true;
        PULSE_LOG_INFO("grid", "{}: spike freeze expired", m_symbol);
    }

    refreshUserPositionWarn();
}

void GridManager::tickSlow(std::int64_t now_ms)
{
    if (GridPhase::Running != m_phase && GridPhase::Protecting != m_phase)
    {
        return;
    }

    reconcileExchange(now_ms);
    manageTp(now_ms);

    const auto action = decideAction(now_ms);
    if (GridAction::None != action)
    {
        executeAction(action, now_ms);
    }
}

// ---------------------------------------------------------------------------
// Rule engine
// ---------------------------------------------------------------------------

double GridManager::computeAtr15m() const
{
    if (nullptr == m_feed)
    {
        return 0.0;
    }
    const auto candles = m_feed->getKlineBuffer(m_symbol).snapshot(
        static_cast<std::size_t>(m_cfg.atr_period) + 1);
    if (candles.size() < 2)
    {
        return 0.0;
    }
    double sum = 0.0;
    int count = 0;
    const auto n = std::min(candles.size(),
                            static_cast<std::size_t>(m_cfg.atr_period) + 1);
    for (std::size_t i = 1; i < n; ++i)
    {
        const auto &c = candles[i];
        const auto &p = candles[i - 1];
        sum += std::max({ c.high - c.low, std::abs(c.high - p.close),
                          std::abs(c.low - p.close) });
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}

std::string GridManager::readTrendGate(std::int64_t /*now_ms*/) const
{
    // Freshness is judged against the WALL clock (nowMs): the board stamps
    // entries with its own clock, which equals the engine's wall clock — but
    // tick() receives a caller-provided timestamp, and tests advance it
    // arbitrarily (spike windows, day boundaries). Judging signal freshness
    // against that synthetic clock would wrongly expire real signals.
    const auto snap = m_board.snapshot();
    const auto &signals = snap.value("signals", nlohmann::json::array());
    const std::string want_id = "eth_scalper_" + m_symbol;
    const std::int64_t now = nowMs();
    for (const auto &sig : signals)
    {
        // SignalBoard keys entries by "source" (the strategy id).
        if (!sig.is_object() || sig.value("source", "") != want_id)
        {
            continue;
        }
        const std::int64_t ts = sig.value("ts_ms", std::int64_t{ 0 });
        if (ts == 0 || now - ts > kSignalFreshMs)
        {
            return "stale"; // expired state — do not hang new levels
        }
        return sig.value("indicators", nlohmann::json::object())
            .value("trend_state", "neutral");
    }
    return "stale";
}

bool GridManager::spikeTripped(std::int64_t now_ms) const
{
    (void)now_ms;
    if (nullptr == m_feed)
    {
        return false;
    }
    const auto candles = m_feed->getKlineBuffer(m_symbol).snapshot(2);
    if (candles.size() < 2 || candles.back().close <= 0.0
        || candles[candles.size() - 2].close <= 0.0)
    {
        return false;
    }
    const double prev = candles[candles.size() - 2].close;
    const double cur = candles.back().close;
    const double gain_pct = (cur - prev) / prev;
    const double atr15 = computeAtr15m();
    const double atr_pct = atr15 > 0.0 ? m_cfg.spike_atr_mult * atr15 / prev : 0.0;
    return gain_pct > std::max(m_cfg.spike_pct, atr_pct);
}

bool GridManager::dailyLossFrozen(std::int64_t /*now_ms*/) const
{
    // The day boundary is rolled by tickFast every tick; this is a pure
    // read of the current window.
    return m_dailyFrozen || m_realizedToday <= m_cfg.daily_loss_limit_usd;
}

double GridManager::computeMid() const
{
    if (nullptr == m_feed)
    {
        return 0.0;
    }
    const auto ticker = m_feed->tickerCache().get(m_symbol);
    if (!ticker.has_value())
    {
        return 0.0;
    }
    if (ticker->bid > 0.0 && ticker->ask > 0.0)
    {
        return (ticker->bid + ticker->ask) / 2.0;
    }
    return ticker->last;
}

GridAction GridManager::decideAction(std::int64_t now_ms) const
{
    if (GridPhase::Running != m_phase)
    {
        return GridAction::None;
    }

    const double mid = computeMid();
    const double top = m_anchor + m_cfg.levels * m_step;
    const double a_line = top + m_cfg.protect_line_a_steps * m_step;

    // Protection line A — 1m close above top + 2×step flattens the grid
    // share (the 15m dimension is subsumed: the 1m close is strictly more
    // sensitive, and the 04:50 flash pump in the review moved 98 points in
    // 2 minutes — a 15m close could never react in time).
    if (nullptr != m_feed)
    {
        const auto candles = m_feed->getKlineBuffer(m_symbol).snapshot(1);
        if (!candles.empty() && candles.back().close > a_line)
        {
            return GridAction::ProtectA;
        }
    }

    // Protection line B — grid share floating loss.
    double grid_loss = 0.0;
    for (const auto &lv : m_levels)
    {
        if (lv.filled > 0 && lv.fill_price > 0.0)
        {
            grid_loss += (lv.fill_price - mid) * lv.filled * kEthQuanto;
        }
    }
    if (grid_loss <= m_cfg.protect_line_b_usd)
    {
        return GridAction::ProtectB;
    }

    // Re-anchor: price fell far below the grid → follow down (cooldown +
    // bearish trend gate). A breakout above the top never auto-re-anchors —
    // it either trips protection A (already checked) or freezes new levels.
    if (mid > 0.0 && mid < m_anchor - m_cfg.lower_reanchor_steps * m_step)
    {
        if (now_ms >= m_reanchorUntil && m_trendGate == "bearish")
        {
            return GridAction::Reanchor;
        }
        return GridAction::FreezeNew;
    }

    // Freeze conditions (new levels only — existing TP/protection still run).
    if (dailyLossFrozen(now_ms) || now_ms < m_spikeUntil
        || m_trendGate != "bearish")
    {
        return GridAction::FreezeNew;
    }

    return GridAction::HangLevels;
}

void GridManager::executeAction(GridAction action, std::int64_t now_ms)
{
    switch (action)
    {
    case GridAction::HangLevels:
        setLastAction(GridAction::HangLevels, now_ms);
        hangLevels();
        break;
    case GridAction::ManageTp:
        setLastAction(GridAction::ManageTp, now_ms);
        manageTp(now_ms);
        break;
    case GridAction::Reanchor:
        setLastAction(GridAction::Reanchor, now_ms);
        reanchor(now_ms, false);
        break;
    case GridAction::ProtectA:
        setLastAction(GridAction::ProtectA, now_ms);
        flattenGridShare(now_ms, "A");
        break;
    case GridAction::ProtectB:
        setLastAction(GridAction::ProtectB, now_ms);
        flattenGridShare(now_ms, "B");
        break;
    case GridAction::FreezeNew:
    case GridAction::DailyStop:
        setLastAction(GridAction::FreezeNew, now_ms);
        break;
    case GridAction::None:
        break;
    }
}

// ---------------------------------------------------------------------------
// Reconciliation + actions
// ---------------------------------------------------------------------------

void GridManager::reconcileExchange(std::int64_t /*now_ms*/)
{
    const auto orders = m_gw.openFuturesOrders(m_symbol);

    // Match exchange orders to levels; detect disappearances (fills/cancels).
    for (auto &lv : m_levels)
    {
        lv.resting = 0;
        lv.tp_resting = false;
    }

    std::map<std::string, std::size_t> seen; // client order id → level idx
    for (const auto &o : orders)
    {
        if (startsWith(o.client_order_id, "eth-grid-sell-"))
        {
            const double price = std::round(o.price * 100.0) / 100.0;
            for (std::size_t i = 0; i < m_levels.size(); ++i)
            {
                if (std::abs(m_levels[i].price - price) < 0.01)
                {
                    ++m_levels[i].resting;
                    m_levelByOrderId[o.order_id] = { i, false };
                    seen[o.client_order_id] = i;
                    break;
                }
            }
        }
        else if (startsWith(o.client_order_id, "eth-grid-tp-"))
        {
            const double fill = std::round(o.price * 100.0) / 100.0;
            for (std::size_t i = 0; i < m_levels.size(); ++i)
            {
                auto &lv = m_levels[i];
                if (std::abs((lv.fill_price - m_cfg.tp_distance_steps * m_step)
                             - fill) < 0.01)
                {
                    lv.tp_resting = true;
                    m_levelByOrderId[o.order_id] = { i, true };
                    break;
                }
            }
        }
    }

    // Orders we tracked that vanished → filled (a limit sell fills at its
    // own price; a TP buy fills at its limit price).
    for (auto it = m_levelByOrderId.begin(); it != m_levelByOrderId.end();)
    {
        const auto &oid = it->first;
        const std::size_t idx = it->second.level_idx;
        const bool is_tp = it->second.is_tp;
        const bool still_live = std::any_of(
            orders.begin(), orders.end(),
            [&oid](const ExchangeOrderView &o) { return o.order_id == oid; });
        if (still_live || idx >= m_levels.size())
        {
            ++it;
            continue;
        }
        auto &lv = m_levels[idx];
        if (m_externalCancelPending)
        {
            // Swept by a direction switch (or manual cancel): NOT a fill.
            PULSE_LOG_INFO("grid", "{}: order {} cancelled externally (not a fill)",
                           m_symbol, oid);
        }
        else if (is_tp)
        {
            // TP first: a resting==0 sell-level must not swallow the TP
            // branch (a TP order never sets lv.resting). The prefix lives in
            // client_order_id, which is gone once the order fills — the
            // tracked type rides in the map instead.
            lv.tp_filled = true;
            lv.tp_resting = false;
            m_stateDirty = true;
            PULSE_LOG_INFO("grid", "{}: TP filled at level {}", m_symbol, lv.price);
        }
        else if (startsWith(oid, "eth-grid-sell-") || lv.resting == 0)
        {
            // A sell order disappeared: treat as a fill. One level order
            // carries the full per-level quantity (2 contracts) — the whole
            // level fills at once, up to the level cap.
            if (lv.filled < static_cast<int>(m_cfg.qty_per_level))
            {
                lv.filled += static_cast<int>(m_cfg.qty_per_level);
                lv.fill_price = lv.price;
                m_stateDirty = true;
                PULSE_LOG_INFO("grid", "{}: level {} filled ({}/{})",
                               m_symbol, lv.price, lv.filled,
                               static_cast<int>(m_cfg.qty_per_level));
            }
        }
        it = m_levelByOrderId.erase(it);
    }
    if (m_externalCancelPending)
    {
        m_externalCancelPending = false;
    }

    // Orders on the exchange we never tracked (restart leftovers) — adopt
    // them so they are managed, not orphaned.
    for (const auto &o : orders)
    {
        if (m_levelByOrderId.contains(o.order_id))
        {
            continue;
        }
        if (startsWith(o.client_order_id, "eth-grid-sell-")
            || startsWith(o.client_order_id, "eth-grid-tp-"))
        {
            // Level resolved by the price match above; the order type from
            // its own client id (it is still on the exchange, so readable).
            m_levelByOrderId[o.order_id] = {
                0, startsWith(o.client_order_id, "eth-grid-tp-") };
            PULSE_LOG_INFO("grid", "{}: adopted external grid order {} ({})",
                           m_symbol, o.order_id, o.client_order_id);
        }
    }
}

void GridManager::hangLevels()
{
    const double mid = computeMid();
    for (auto &lv : m_levels)
    {
        if (lv.resting > 0 || lv.filled > 0 || lv.price <= 0.0)
        {
            continue;
        }
        if (mid > 0.0 && lv.price <= mid)
        {
            continue; // grid sits above price — wait for the rebound
        }

        execution::OrderRequest req;
        req.symbol = m_symbol;
        req.side = Side::Sell;
        req.quantity = static_cast<double>(m_cfg.qty_per_level);
        req.type = OrderType::Limit;
        req.price = lv.price;
        req.market_type = MarketType::Futures;
        req.client_order_id = sellId(lv.price);
        req.quanto_multiplier = kEthQuanto;

        const auto res = m_gw.place(req);
        if (ok(res))
        {
            m_levelByOrderId[value(res).order_id] = {
                static_cast<std::size_t>(std::distance(m_levels.data(), &lv)),
                false };
            ++lv.resting;
            m_stateDirty = true;
            PULSE_LOG_INFO("grid", "{}: hung level {} x{} ({})",
                           m_symbol, lv.price, m_cfg.qty_per_level,
                           value(res).order_id);
        }
        else
        {
            PULSE_LOG_WARN("grid", "{}: level {} rejected: {}",
                           m_symbol, lv.price, error(res).message);
            // One rejection per slow pass is enough — do not hammer the gate.
            return;
        }
    }
}

void GridManager::manageTp(std::int64_t now_ms)
{
    const double mid = computeMid();
    for (auto &lv : m_levels)
    {
        // Filled level without a resting TP → hang the reduce-only TP buy.
        if (lv.filled > 0 && !lv.tp_resting && !lv.tp_filled)
        {
            const double tp_price = lv.fill_price - m_cfg.tp_distance_steps * m_step;
            execution::OrderRequest req;
            req.symbol = m_symbol;
            req.side = Side::Buy;
            req.quantity = static_cast<double>(lv.filled);
            req.type = OrderType::Limit;
            req.price = tp_price;
            req.market_type = MarketType::Futures;
            req.reduce_only = true; // M27: never a trigger order (whole-position only)
            req.client_order_id = tpId(lv.fill_price);
            req.quanto_multiplier = kEthQuanto;

            const auto res = m_gw.place(req);
            if (ok(res))
            {
                lv.tp_resting = true;
                m_levelByOrderId[value(res).order_id] = {
                    static_cast<std::size_t>(std::distance(m_levels.data(), &lv)),
                    true };
                m_stateDirty = true;
                PULSE_LOG_INFO("grid", "{}: TP for fill {} → {}",
                               m_symbol, lv.fill_price, tp_price);
            }
            else
            {
                PULSE_LOG_WARN("grid", "{}: TP for {} rejected: {}",
                               m_symbol, lv.price, error(res).message);
            }
        }

        // TP filled → realized PnL, then cyclic re-hang when price is below.
        if (lv.tp_filled)
        {
            const double tp_price = lv.fill_price - m_cfg.tp_distance_steps * m_step;
            recordTpFill(lv.fill_price, tp_price, lv.filled);
            lv.tp_filled = false;
            lv.tp_resting = false;
            lv.filled = 0;
            lv.fill_price = 0.0;
            if (mid > 0.0 && mid < lv.price)
            {
                // Cyclic re-hang — the classic grid loop. Level state is
                // zeroed; the next slow pass re-hangs it (trend-gated).
                m_stateDirty = true;
                PULSE_LOG_INFO("grid", "{}: level {} re-hung for the next cycle",
                               m_symbol, lv.price);
            }
        }
    }
}

void GridManager::reanchor(std::int64_t now_ms, bool protect_triggered)
{
    cancelAllGridOrders();
    m_levels.clear();
    m_levelByOrderId.clear();
    recomputeLevels(now_ms);
    hangLevels();
    m_reanchorUntil = now_ms
        + static_cast<std::int64_t>(m_cfg.reanchor_cooldown_min) * 60000;
    m_stateDirty = true;
    PULSE_LOG_INFO("grid", "{}: re-anchored at {} (step {}, protect={})",
                   m_symbol, m_anchor, m_step, protect_triggered ? "yes" : "no");
}

void GridManager::flattenGridShare(std::int64_t now_ms, const char *line)
{
    int share = 0;
    for (const auto &lv : m_levels)
    {
        share += lv.filled;
    }
    if (share <= 0)
    {
        return;
    }

    // Reduce-only market buy for EXACTLY the grid share — never more (the
    // merged user share must not be touched beyond our own accounting).
    execution::OrderRequest req;
    req.symbol = m_symbol;
    req.side = Side::Buy;
    req.quantity = static_cast<double>(share);
    req.type = OrderType::Market;
    req.market_type = MarketType::Futures;
    req.reduce_only = true;
    req.client_order_id = "eth-grid-flatten-" + std::to_string(now_ms);
    req.quanto_multiplier = kEthQuanto;

    const auto res = m_gw.place(req);
    if (ok(res))
    {
        m_lastProtectMs = now_ms;
        PULSE_LOG_WARN("grid", "{}: protection line {} — flattened grid share {} "
                       "contracts, re-anchoring", m_symbol, line, share);
    }
    else
    {
        PULSE_LOG_WARN("grid", "{}: protection line {} flatten rejected: {}",
                       m_symbol, line, error(res).message);
        return;
    }

    // Realized loss of the flattened share (best-effort at current mid —
    // the actual market fill price arrives via the order report later).
    const double mid = computeMid();
    double loss = 0.0;
    for (const auto &lv : m_levels)
    {
        if (lv.filled > 0 && lv.fill_price > 0.0 && mid > 0.0)
        {
            loss += (lv.fill_price - mid) * lv.filled * kEthQuanto;
        }
    }
    recordFlattenLoss(loss);

    // Zero the share ledger and rebuild levels per the rules (trend-gated).
    for (auto &lv : m_levels)
    {
        lv.filled = 0;
        lv.fill_price = 0.0;
        lv.tp_filled = false;
        lv.tp_resting = false;
    }
    cancelAllGridOrders();
    m_levelByOrderId.clear();
    recomputeLevels(now_ms);
    hangLevels();
    m_stateDirty = true;
}

void GridManager::recordFlattenLoss(double loss_usd)
{
    if (loss_usd >= 0.0)
    {
        return; // only losses count against the daily window
    }
    m_realizedToday += loss_usd;
    m_stateDirty = true;
    PULSE_LOG_WARN("grid", "{}: protection flatten realized {:.2f} USD "
                   "(today {:.2f})", m_symbol, loss_usd, m_realizedToday);
    if (m_realizedToday <= m_cfg.daily_loss_limit_usd && !m_dailyFrozen)
    {
        m_dailyFrozen = true;
        m_stateDirty = true;
        PULSE_LOG_WARN("grid", "{}: daily loss stop — realized {:.2f} ≤ {:.2f}, "
                       "new levels frozen", m_symbol, m_realizedToday,
                       m_cfg.daily_loss_limit_usd);
    }
}

void GridManager::cancelAllGridOrders()
{
    const auto orders = m_gw.openFuturesOrders(m_symbol);
    for (const auto &o : orders)
    {
        if (startsWith(o.client_order_id, "eth-grid-"))
        {
            m_gw.cancel(o.order_id);
        }
    }
}

void GridManager::recordTpFill(double sell_price, double tp_fill_price, double qty)
{
    const double pnl = (sell_price - tp_fill_price) * qty * kEthQuanto;
    m_realizedToday += pnl;
    m_stateDirty = true;
    PULSE_LOG_INFO("grid", "{}: TP realized {} USD (today {:.2f})",
                   m_symbol, pnl, m_realizedToday);
    if (m_realizedToday <= m_cfg.daily_loss_limit_usd && !m_dailyFrozen)
    {
        m_dailyFrozen = true;
        m_stateDirty = true;
        PULSE_LOG_WARN("grid", "{}: daily loss stop — realized {:.2f} ≤ {:.2f}, "
                       "new levels frozen", m_symbol, m_realizedToday,
                       m_cfg.daily_loss_limit_usd);
    }
}

void GridManager::recomputeLevels(std::int64_t now_ms)
{
    // Step: ATR-adaptive (clamp(0.5×ATR15m, min, max)) or fixed.
    if (m_cfg.step_mode == "atr")
    {
        const double atr = computeAtr15m();
        const double raw = m_cfg.step_atr_mult * atr;
        m_step = std::clamp(std::round(raw), m_cfg.step_min, m_cfg.step_max);
        if (m_step <= 0.0)
        {
            m_step = m_cfg.step_fixed > 0.0 ? m_cfg.step_fixed : 5.0;
        }
    }
    else
    {
        m_step = m_cfg.step_fixed;
    }

    // Anchor: round(mid + 1×step, step). Grid sits ABOVE price (chase-short
    // regime — the grid waits for a rebound into the level band).
    const double mid = computeMid();
    if (mid > 0.0 && m_step > 0.0)
    {
        m_anchor = roundToStep(mid + m_cfg.anchor_offset_steps * m_step, m_step);
    }
    else
    {
        m_anchor = 0.0;
    }

    m_levels.clear();
    for (int i = 0; i < m_cfg.levels; ++i)
    {
        GridLevel lv;
        lv.price = m_anchor + static_cast<double>(i) * m_step;
        m_levels.push_back(lv);
    }
    m_levelByOrderId.clear();
    m_stateDirty = true;
    PULSE_LOG_INFO("grid", "{}: levels recomputed (anchor {}, step {}, {} levels, "
                   "now_ms {})", m_symbol, m_anchor, m_step, m_cfg.levels, now_ms);
}

void GridManager::refreshUserPositionWarn()
{
    const auto positions = m_gw.positionsBySymbol(m_symbol);
    bool warn = false;
    for (const auto &p : positions)
    {
        if (!startsWith(p.strategy_id, "eth-grid-"))
        {
            warn = true;
            break;
        }
    }
    m_userPositionWarn = warn;
}

void GridManager::setLastAction(GridAction action, std::int64_t now_ms)
{
    static const char *kNames[] = { "None", "HangLevels", "ManageTp",
                                    "Reanchor", "ProtectA", "ProtectB",
                                    "FreezeNew", "DailyStop" };
    m_lastActionName = kNames[static_cast<int>(action)];
    m_lastActionMs = now_ms;
}

void GridManager::markSpikeIfNeeded(std::int64_t now_ms)
{
    // While a freeze is active, the same (or new) spike candles must not
    // keep extending it — the freeze runs its configured window once, then
    // re-evaluates fresh data.
    if (now_ms < m_spikeUntil || !spikeTripped(now_ms))
    {
        return;
    }
    m_spikeUntil = now_ms
        + static_cast<std::int64_t>(m_cfg.spike_freeze_min) * 60000;
    m_stateDirty = true;
    PULSE_LOG_WARN("grid", "{}: spike detected — new levels frozen for {} min",
                   m_symbol, m_cfg.spike_freeze_min);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

bool GridManager::loadState()
{
    std::lock_guard lock{ m_mutex };
    std::ifstream in{ m_stateFile };
    if (!in)
    {
        return false;
    }
    nlohmann::json j;
    try
    {
        in >> j;
    }
    catch (const std::exception &)
    {
        PULSE_LOG_WARN("grid", "{}: state file unreadable — starting fresh",
                       m_symbol);
        return false;
    }

    m_anchor = j.value("anchor", 0.0);
    m_step = j.value("step", m_cfg.step_fixed);
    m_realizedToday = j.value("realized_pnl_today", 0.0);
    m_dayStartSec = j.value("day_start_sec", std::int64_t{ 0 });
    m_spikeUntil = j.value("spike_until_ms", std::int64_t{ 0 });
    m_reanchorUntil = j.value("reanchor_until_ms", std::int64_t{ 0 });
    m_dailyFrozen = j.value("daily_frozen", false);

    m_levels.clear();
    for (const auto &lj : j.value("levels", nlohmann::json::array()))
    {
        GridLevel lv;
        lv.price = lj.value("price", 0.0);
        lv.filled = lj.value("filled", 0);
        lv.fill_price = lj.value("fill_price", 0.0);
        lv.tp_filled = lj.value("tp_filled", false);
        m_levels.push_back(lv);
    }
    if (m_levels.empty())
    {
        recomputeLevels(nowMs());
    }
    m_levelByOrderId.clear();
    PULSE_LOG_INFO("grid", "{}: state loaded (anchor {}, step {}, realized {:.2f})",
                   m_symbol, m_anchor, m_step, m_realizedToday);
    return true;
}

bool GridManager::saveState() const
{
    nlohmann::json j;
    j["schema"] = 1;
    j["symbol"] = m_symbol;
    j["anchor"] = m_anchor;
    j["step"] = m_step;
    j["realized_pnl_today"] = m_realizedToday;
    j["day_start_sec"] = m_dayStartSec;
    j["spike_until_ms"] = m_spikeUntil;
    j["reanchor_until_ms"] = m_reanchorUntil;
    j["daily_frozen"] = m_dailyFrozen;
    nlohmann::json levels = nlohmann::json::array();
    for (const auto &lv : m_levels)
    {
        levels.push_back({ { "price", lv.price },
                           { "filled", lv.filled },
                           { "fill_price", lv.fill_price },
                           { "tp_filled", lv.tp_filled } });
    }
    j["levels"] = levels;

    // Atomic write: tmp + rename so a crash mid-write never corrupts state.
    try
    {
        std::filesystem::create_directories(m_stateFile.parent_path());
        const auto tmp = m_stateFile.string() + ".tmp";
        {
            std::ofstream out{ tmp };
            if (!out)
            {
                return false;
            }
            out << j.dump(2);
        }
        std::filesystem::rename(tmp, m_stateFile);
    }
    catch (const std::exception &)
    {
        return false;
    }
    return true;
}

nlohmann::json gridSnapshotToJson(const GridSnapshot &snap)
{
    nlohmann::json levels = nlohmann::json::array();
    for (const auto &lv : snap.levels)
    {
        levels.push_back({ { "price", lv.price },
                           { "resting", lv.resting },
                           { "filled", lv.filled },
                           { "fill_price", lv.fill_price },
                           { "tp_resting", lv.tp_resting },
                           { "tp_filled", lv.tp_filled } });
    }
    return nlohmann::json{
        { "phase", static_cast<int>(snap.phase) },
        { "phase_name",
          snap.phase == GridPhase::Running    ? "running"
          : snap.phase == GridPhase::Paused   ? "paused"
          : snap.phase == GridPhase::Protecting ? "protecting"
                                              : "disabled" },
        { "symbol", snap.symbol },
        { "anchor", snap.anchor },
        { "step", snap.step },
        { "top", snap.top },
        { "levels_filled", snap.levels_filled },
        { "realized_pnl_today", snap.realized_pnl_today },
        { "unrealized_pnl", snap.unrealized_pnl },
        { "trend_gate", snap.trend_gate },
        { "spike_frozen", snap.spike_frozen },
        { "spike_until_ms", snap.spike_until_ms },
        { "daily_loss_frozen", snap.daily_loss_frozen },
        { "reanchor_until_ms", snap.reanchor_until_ms },
        { "user_position_warn", snap.user_position_warn },
        { "last_action", snap.last_action },
        { "last_action_ms", snap.last_action_ms },
        { "levels", levels },
    };
}

} // namespace pulse::grid
