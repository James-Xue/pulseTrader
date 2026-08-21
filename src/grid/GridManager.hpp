#pragma once
// grid_manager.hpp — engine-native grid execution service (M27)
//
// Runs a short-only futures grid (the ETH grid v2 rules from
// commit_my_life gate交易/以太坊 策略文档 v2) inside the engine, replacing
// the Python toolchain:
//   - levels of limit sell orders above the anchor, 2 contracts each
//   - per-level reduce-only limit TP buy at fill - 2×step (never trigger
//     orders — Gate price_orders only close whole positions)
//   - TP fill → cyclic re-hang (same level when price is below it)
//   - re-anchor with 30-min cooldown (follow down; a breakout through the
//     top goes to protection line A instead)
//   - trend gate: new levels only in a bearish EMA regime (SignalBoard
//     eth_scalper trend_state, or self-computed 15m EMA when stale/missing)
//   - spike freeze: 1m gain > max(1%, 3×ATR15m) freezes new levels 30 min
//   - protection line A: 15m OR 1m close > top + 2×step → flatten the grid
//     share (levels_filled × qty), then re-anchor per the rules
//   - protection line B: grid floating loss ≤ -30 USD → same
//   - daily loss stop: realized ≤ -10 USD freezes new levels; Beijing-day
//     reset (08:00 CST == 00:00 UTC — the two coincide for UTC+8)
//
// Grid share accounting is ledger-level only (levels_filled): Gate one-way
// futures merge the grid short with any user short on the same contract, so
// reduce-only closes can never target "only the grid part" — a structural
// limitation documented in the strategy doc, surfaced via user_position_warn.
//
// Threading: NO own thread — the main loop calls tick() every 200ms (the
// same slot as OrderFlowExecutor::sweepMakerAttempts). Internal layering:
// fast (every tick) / mid (~1s) / slow (~50s, the reconciliation main pass).
// All state is guarded by m_mutex; control-plane methods (start/status/
// pause/stop) take the same lock from their own threads. Lock order:
// m_mutex → rest_mutex (never the reverse).

#include "core/PulseError.hpp"
#include "core/config.hpp"
#include "grid/IGridGateway.hpp"
#include "market/MarketFeed.hpp"
#include "strategy/signal/SignalBoard.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::grid
{

enum class GridPhase : int
{
    Disabled,   ///< Stopped / never started — manages nothing.
    Paused,     ///< grid_pause: orders stay, no new action.
    Running,    ///< Normal: periodic reconcile / TP / re-hang.
    Protecting, ///< Protection-line flatten in progress (transient).
};

enum class GridAction : int
{
    None,
    HangLevels,  ///< Normal path: fill missing level orders (trend-gated).
    ManageTp,    ///< Filled level → hang TP; TP filled → re-hang.
    Reanchor,    ///< Follow-down re-anchor (cooldown + bearish gate).
    ProtectA,    ///< 15m/1m close above top + 2×step → flatten grid share.
    ProtectB,    ///< Grid floating loss ≤ -30 USD → flatten grid share.
    FreezeNew,   ///< Frozen (spike / daily loss / bullish trend) — manage存量 only.
    DailyStop,   ///< Daily realized ≤ -10 → freeze new levels.
};

/// One grid level's lifecycle (ledger-level share accounting).
struct GridLevel
{
    double price{ 0.0 };        ///< Level price (the short limit price).
    int resting{ 0 };           ///< Open sell orders at this level (0-2).
    int filled{ 0 };            ///< Contracts filled (0-2).
    double fill_price{ 0.0 };   ///< Weighted average fill (TP base).
    bool tp_resting{ false };   ///< A reduce-only TP buy is resting.
    bool tp_filled{ false };    ///< TP filled this cycle (re-hang pending).
};

/// Serialization view for grid_status.
struct GridSnapshot
{
    GridPhase phase{ GridPhase::Disabled };
    std::string symbol;
    double anchor{ 0.0 };
    double step{ 0.0 };
    double top{ 0.0 };
    int levels_filled{ 0 };
    double realized_pnl_today{ 0.0 };
    double unrealized_pnl{ 0.0 };
    std::string trend_gate{ "unknown" }; ///< bearish / bullish / neutral / stale
    bool spike_frozen{ false };
    std::int64_t spike_until_ms{ 0 };
    bool daily_loss_frozen{ false };
    std::int64_t reanchor_until_ms{ 0 };
    bool user_position_warn{ false };
    std::string last_action;             ///< Last GridAction name.
    std::int64_t last_action_ms{ 0 };
    std::vector<GridLevel> levels;
};

class GridManager
{
  public:
    GridManager(const GridConfig &cfg, IGridGateway &gw,
                strategy::SignalBoard &board, market::MarketFeed *futures_feed,
                std::mutex &rest_mutex,
                std::filesystem::path state_dir = std::filesystem::path{ "data" });

    // --- Control plane (thread-safe; same m_mutex as tick) ---

    /// Start the grid. Optional JSON overrides (levels/qty/step/anchor).
    /// Rejected when the symbol has non-grid positions unless cfg.force.
    [[nodiscard]] Result<GridSnapshot> start(const nlohmann::json &overrides);
    [[nodiscard]] GridSnapshot status() const;
    [[nodiscard]] Result<GridSnapshot> pause();
    [[nodiscard]] Result<GridSnapshot> stop();

    /// Periodic reconciliation — main loop, every ~200ms.
    void tick(std::int64_t now_ms);

    /// Engine direction switch: futures orders may have been swept by
    /// cancelAllOpenOrders — next slow pass re-hangs (trend-gated).
    void onDirectionSwitched();

    // --- Persistence ---
    bool loadState();
    bool saveState() const;

  private:
    /// Lock-free snapshot builder — callers must hold m_mutex.
    [[nodiscard]] GridSnapshot statusLocked() const;

    // Tick layering (tick is called every 200ms by the main loop).
    void tickFast(std::int64_t now_ms);
    void tickMid(std::int64_t now_ms);
    void tickSlow(std::int64_t now_ms);

    // Rule engine (pure helpers, unit-test friendly).
    [[nodiscard]] double computeAtr15m() const;
    [[nodiscard]] std::string readTrendGate(std::int64_t now_ms) const;
    [[nodiscard]] bool spikeTripped(std::int64_t now_ms) const;
    [[nodiscard]] bool dailyLossFrozen(std::int64_t now_ms) const;
    [[nodiscard]] double computeMid() const;
    [[nodiscard]] GridAction decideAction(std::int64_t now_ms) const;

    void executeAction(GridAction action, std::int64_t now_ms);
    void reconcileExchange(std::int64_t now_ms);
    void hangLevels();
    void manageTp(std::int64_t now_ms);
    void reanchor(std::int64_t now_ms, bool protect_triggered);
    void flattenGridShare(std::int64_t now_ms, const char *line);
    void cancelAllGridOrders();
    void recordTpFill(double sell_price, double tp_fill_price, double qty);
    void recordFlattenLoss(double loss_usd);
    void recomputeLevels(std::int64_t now_ms);
    void refreshUserPositionWarn();
    void setLastAction(GridAction action, std::int64_t now_ms);
    void markSpikeIfNeeded(std::int64_t now_ms);

    // --- Members ---
    const GridConfig &m_cfg;
    IGridGateway &m_gw;
    strategy::SignalBoard &m_board;
    market::MarketFeed *m_feed; ///< Futures feed (nullable in tests without one).
    std::mutex &m_restMutex;    ///< Shared REST serialization (unused by grid directly).
    std::filesystem::path m_stateFile;

    mutable std::mutex m_mutex;
    GridPhase m_phase{ GridPhase::Disabled };
    std::string m_symbol;
    double m_anchor{ 0.0 };
    double m_step{ 0.0 };
    struct TrackedOrder
    {
        std::size_t level_idx{ 0 }; ///< Index into m_levels.
        bool is_tp{ false };        ///< TP buy vs level sell (prefix lives in
                                    ///< client_order_id, which vanish on fill —
                                    ///< the type must ride in the map).
    };
    std::vector<GridLevel> m_levels; ///< levels entries, index 0 = lowest.
    std::map<std::string, TrackedOrder> m_levelByOrderId; ///< Exchange id → tracked.
    double m_realizedToday{ 0.0 };
    std::int64_t m_dayStartSec{ 0 };
    std::int64_t m_spikeUntil{ 0 };
    std::int64_t m_reanchorUntil{ 0 };
    std::int64_t m_lastProtectMs{ 0 };
    bool m_dailyFrozen{ false };
    bool m_userPositionWarn{ false };
    bool m_externalCancelPending{ false }; ///< Switch swept orders — treat disappearances as cancels.
    std::string m_trendGate{ "unknown" };
    std::string m_lastActionName{ "None" };
    std::int64_t m_lastActionMs{ 0 };
    std::int64_t m_lastTickMs{ 0 };
    int m_tickCounter{ 0 };
    bool m_stateDirty{ false };
};

/// Serialize a GridSnapshot for the control plane (grid_status JSON).
[[nodiscard]] nlohmann::json gridSnapshotToJson(const GridSnapshot &snap);

} // namespace pulse::grid
