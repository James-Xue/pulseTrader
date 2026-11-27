// test_paper_engine_flow.cpp — integration: L9 control → L8 flow → paper venue → recorder.
//
// Unlike the unit suites, this file drives the real wiring end to end: a
// control-plane order reaches the order flow, the simulated venue fills it,
// the engine opens a position, the venue exits it on take-profit, and
// get_trade_history plus the account identity must reconcile afterwards.
//
// SQLite-only: the trade history and the PnL-bearing ledger are the point.

#include "control/EngineServices.hpp"
#include "control/OrderFlowExecutor.hpp"
#include "core/config.hpp"
#include "execution/PaperAccount.hpp"
#include "execution/PaperVenue.hpp"
#include "execution/OrderTracker.hpp"
#include "exchange/GateRestClient.hpp"
#include "risk/DrawdownGuard.hpp"
#include "risk/OrderRateLimiter.hpp"
#include "risk/PositionManager.hpp"
#include "risk/RiskManager.hpp"
#include "strategy/signal/SignalBoard.hpp"
#include "strategy/StrategyManager.hpp"
#include "trade_recorder/TradeRecorder.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>

using namespace pulse;
using namespace pulse::control;
using namespace pulse::execution;
using namespace pulse::risk;

namespace
{

class PaperEngineFlowTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_paperCfg.initial_balance_usdt = 100.0;
        m_paperCfg.contract_volume["XAUUSD"] = 100.0;
        m_paperCfg.stop_out_enabled = false;
        m_stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

        m_account = std::make_unique<PaperAccount>(m_paperCfg);
        m_venue = std::make_unique<PaperVenue>(
            m_paperCfg, std::vector<Symbol>{ "XAUUSD" }, *m_account);
        m_venue->setMarkProvider(
            [this](const Symbol &, MarketType) -> std::optional<PaperMark>
            { return PaperMark{ m_price, m_stamp, m_price, m_price }; });

        const auto *test_info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        m_dbPath = (std::filesystem::temp_directory_path()
                    / (std::string("pulse_it_") + test_info->name() + ".db"))
                       .string();
        std::error_code ec;
        std::filesystem::remove(m_dbPath, ec);
        auto recorder = trade_recorder::TradeRecorder::open(m_dbPath);
        ASSERT_TRUE(ok(recorder)) << error(recorder).message;
        m_recorder = std::make_unique<trade_recorder::TradeRecorder>(
            std::move(value(recorder)));

        m_riskCfg.maxPositionNotional = 20000.0;
        m_riskCfg.maxSymbolNotional = 20000.0;
        m_riskCfg.maxPositionNotionalCfd = 20000.0;
        m_riskCfg.maxOpenPositions = 5;

        m_positionMgr = std::make_unique<PositionManager>(m_riskCfg);
        m_drawdownGuard = std::make_unique<DrawdownGuard>(m_riskCfg);
        m_rateLimiter =
            std::make_unique<OrderRateLimiter>(m_riskCfg.maxOrdersPerSec);
        m_riskMgr = std::make_unique<RiskManager>(
            m_riskCfg, *m_positionMgr, *m_drawdownGuard, *m_rateLimiter);
        m_board = std::make_unique<strategy::SignalBoard>(
            m_strategyCfg.signal_aggregator_threshold);
        m_restClient = std::make_unique<exchange::GateRestClient>(m_exchangeCfg);

        m_flow = std::make_unique<OrderFlowExecutor>(
            m_strategyCfg, *m_riskMgr, *m_positionMgr, *m_drawdownGuard,
            nullptr, nullptr, m_venue.get(),
            nullptr, nullptr, m_venue.get(),
            nullptr, nullptr, m_restMutex, m_recorder.get());

        m_services = std::make_unique<EngineServices>(
            "it", m_start, m_cfg, m_strategyMgr, *m_riskMgr, *m_positionMgr,
            nullptr, nullptr, nullptr,
            nullptr, nullptr, m_restClient.get(),
            nullptr, nullptr, m_venue.get(),
            *m_flow, *m_board, m_restMutex,
            nullptr, nullptr, nullptr, m_recorder.get(), m_venue.get());
    }

    void TearDown() override
    {
        m_services.reset();
        m_flow.reset();
        m_recorder.reset();
        std::error_code ec;
        std::filesystem::remove(m_dbPath, ec);
        std::filesystem::remove(m_dbPath + "-wal", ec);
        std::filesystem::remove(m_dbPath + "-shm", ec);
        std::filesystem::remove(m_dbPath + ".state.json", ec);
        std::filesystem::remove(m_dbPath + ".state.json.capital.jsonl", ec);
    }

    PaperConfig m_paperCfg;
    ExchangeConfig m_exchangeCfg;
    PulseConfig m_cfg;
    RiskConfig m_riskCfg;
    StrategyConfig m_strategyCfg;
    strategy::StrategyManager m_strategyMgr;
    std::chrono::steady_clock::time_point m_start =
        std::chrono::steady_clock::now();
    std::mutex m_restMutex;
    double m_price{ 4000.0 };
    std::int64_t m_stamp{ 0 };
    std::string m_dbPath;

    std::unique_ptr<PaperAccount> m_account;
    std::unique_ptr<PaperVenue> m_venue;
    std::unique_ptr<trade_recorder::TradeRecorder> m_recorder;
    std::unique_ptr<PositionManager> m_positionMgr;
    std::unique_ptr<DrawdownGuard> m_drawdownGuard;
    std::unique_ptr<OrderRateLimiter> m_rateLimiter;
    std::unique_ptr<RiskManager> m_riskMgr;
    std::unique_ptr<strategy::SignalBoard> m_board;
    std::unique_ptr<exchange::GateRestClient> m_restClient;
    std::unique_ptr<OrderFlowExecutor> m_flow;
    std::unique_ptr<EngineServices> m_services;
};

TEST_F(PaperEngineFlowTest, OrderToExitReconcilesAcrossHistoryAndIdentity)
{
    m_flow->setActiveMarket(MarketType::Cfd);

    // 1. Control-plane order → flow → venue fill → engine position.
    OrderRequest request;
    request.symbol = "XAUUSD";
    request.side = Side::Buy;
    request.type = OrderType::Market;
    request.quantity = 0.01;
    request.market_type = MarketType::Cfd;
    request.leverage = 1000.0;
    request.quanto_multiplier = 100.0;
    request.client_order_id = "xau_team:xau_e2e:000001";
    request.reference_price = 4000.0;
    request.tp_price = 4010.0;
    const auto placed = m_flow->placeOrder(request);
    ASSERT_TRUE(ok(placed)) << error(placed).message;
    ASSERT_EQ(1u, m_venue->openPositionCount());
    ASSERT_EQ(1u, m_positionMgr->getPositionsBySymbol("XAUUSD").size());

    // 2. The take-profit fires; the venue closes and L9 records the row.
    m_price = 4010.0;
    m_services->tickPaperVenue(m_stamp + 1);
    EXPECT_EQ(0u, m_venue->openPositionCount());
    EXPECT_TRUE(m_positionMgr->getPositionsBySymbol("XAUUSD").empty());

    // 3. History: one entry + one exit, both linked to the venue position.
    const auto history = m_services->tradeHistory(nlohmann::json::object());
    ASSERT_TRUE(ok(history)) << error(history).message;
    const auto &rows = value(history)["rows"];
    ASSERT_EQ(2u, rows.size());
    const auto &exit_row = rows[0];
    const auto &entry_row = rows[1];
    EXPECT_EQ("paper-cfd-1", entry_row["order_id"]);
    EXPECT_EQ(0.0, entry_row["pnl"].get<double>());
    EXPECT_NEAR(0.06, entry_row["fees"].get<double>(), 1e-9); // entry BUY leg
    EXPECT_FALSE(entry_row["exchange_position_id"].get<std::string>().empty());
    EXPECT_EQ(0u, exit_row["order_id"].get<std::string>().rfind("paper_exit_", 0));
    EXPECT_NEAR(10.0, exit_row["pnl"].get<double>(), 1e-9);
    EXPECT_NEAR(0.0, exit_row["fees"].get<double>(), 1e-9); // SELL leg is free

    // 4. Account identity: 100 - 0.06 (entry) + 10.00 (TP) - 0.00 (sell leg).
    const auto snapshot = m_account->snapshot();
    EXPECT_NEAR(109.94, snapshot.cfd.cash_usdt, 1e-9);
    EXPECT_NEAR(0.0, m_account->identityResidual(PaperLeg::Cfd), 1e-9);
}

} // namespace
