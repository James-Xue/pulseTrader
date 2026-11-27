// test_reconciliation_contract.cpp — P0 of the 2026-11-27 paper-team reconciliation audit.
//
// These tests are CANARIES, not red-first tests: they assert the frozen evidence under
// tests/fixtures/reconciliation (never the live data/ files). A failure here means the
// fixture was replaced or edited, or the epoch helper drifted from
// docs/audits/paper-team-reconciliation-2026-11-27.md §2 — not that a behaviour
// regression was caught. The red-first tests for the actual defects are P1..P5.
//
// Why fixtures instead of data/:
// 1. The paper engine was still running when the audit closed; data/trades-paper.db
//    keeps growing and its -wal file is live.
// 2. P0 requires live files to stay read-only; a test that opened data/ could observe
//    a half-written WAL and flake.
// 3. The fixtures are committed, so the contract survives a machine wipe.

#include "reconciliation_epochs.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifdef PULSE_ENABLE_SQLITE
#include <SQLiteCpp/SQLiteCpp.h>
#endif

namespace
{

using namespace pulse::test::reconciliation;

/// Absolute path injected by CMakeLists.txt; tests never guess a relative path
/// because CTest runs with the build directory as the working directory.
const std::filesystem::path kFixtureDir{PULSE_RECONCILIATION_FIXTURE_DIR};

/// Load one fixture JSON file. A missing file is a hard fixture failure: it must not
/// degrade into "zero values", which would make the identity below trivially true.
nlohmann::json loadJson(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    if (!stream)
    {
        ADD_FAILURE() << "cannot open fixture: " << path;
        return nlohmann::json::object();
    }
    return nlohmann::json::parse(stream);
}

/// Per-epoch accumulator used by the trade-fixture test.
struct EpochAccumulator
{
    int rows = 0;
    double pnl = 0.0;
    double fees = 0.0;
    std::int64_t minId = 0;
    std::int64_t maxId = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. The frozen boundaries themselves.
// ---------------------------------------------------------------------------
TEST(ReconciliationContract, EpochBoundariesAreFrozen)
{
    // 1. Four epochs, each non-empty.
    // 2. Consecutive epochs are contiguous: the exclusive end of one is exactly the
    //    start of the next, so no instant belongs to two epochs or none.
    // 3. Only E3 closes inclusively (its end is the 12:53 snapshot instant).
    ASSERT_EQ(4u, kEpochs.size());
    for (std::size_t index = 0; index < kEpochs.size(); ++index)
    {
        SCOPED_TRACE(kEpochs[index].id);
        EXPECT_LT(kEpochs[index].startNs, kEpochs[index].endNs);
        if (0 < index)
        {
            EXPECT_EQ(kEpochs[index - 1].endNs, kEpochs[index].startNs);
        }
    }
    EXPECT_FALSE(kEpochs[0].endInclusive);
    EXPECT_FALSE(kEpochs[1].endInclusive);
    EXPECT_FALSE(kEpochs[2].endInclusive);
    EXPECT_TRUE(kEpochs[3].endInclusive);

    // Independent literals: derived with `date -u -d "<CST wall clock minus 8 h>" +%s`
    // and multiplied by 1e9. They are deliberately NOT produced by cstToUnixNs, so a
    // broken civil-date conversion fails here instead of agreeing with itself.
    EXPECT_EQ(1795591935000000000LL, kEpochs[0].startNs); // 2026-11-25 15:32:15 CST
    EXPECT_EQ(1795660200000000000LL, kEpochs[0].endNs);   // 2026-11-26 10:30:00 CST
    EXPECT_EQ(1795742580000000000LL, kEpochs[1].endNs);   // 2026-11-27 09:23:00 CST
    EXPECT_EQ(1795743960000000000LL, kEpochs[2].endNs);   // 2026-11-27 09:46:00 CST
    EXPECT_EQ(1795755180000000000LL, kEpochs[3].endNs);   // 2026-11-27 12:53:00 CST
}

TEST(ReconciliationContract, ClassifyEpochHonoursHalfOpenBoundaries)
{
    // A boundary instant belongs to the epoch that starts there; the instant one
    // nanosecond before it belongs to the previous epoch. E3 is the sole exception:
    // its closing snapshot instant stays inside E3.
    EXPECT_EQ(0, classifyEpoch(kEpochs[0].startNs));
    EXPECT_EQ(0, classifyEpoch(kEpochs[0].endNs - 1));
    EXPECT_EQ(1, classifyEpoch(kEpochs[0].endNs));
    EXPECT_EQ(2, classifyEpoch(kEpochs[1].endNs));
    EXPECT_EQ(3, classifyEpoch(kEpochs[2].endNs));
    EXPECT_EQ(3, classifyEpoch(kEpochs[3].endNs));
    EXPECT_EQ(-1, classifyEpoch(kEpochs[3].endNs + 1));
    EXPECT_EQ(-1, classifyEpoch(kEpochs[0].startNs - 1));
}

// ---------------------------------------------------------------------------
// 2. Every file-backed snapshot reconciles: cash + locked = seed + realized - fees.
// ---------------------------------------------------------------------------
TEST(ReconciliationContract, SnapshotVenueIdentityHolds)
{
    for (const auto &snapshot : kSnapshots)
    {
        SCOPED_TRACE(snapshot.epochId);
        const nlohmann::json state = loadJson(kFixtureDir / snapshot.file);
        ASSERT_TRUE(state.contains("account")) << "fixture has no account block";
        ASSERT_TRUE(state["account"].contains("cfd")) << "fixture has no cfd leg";

        const nlohmann::json &cfd = state["account"]["cfd"];
        const double cash = cfd.at("cash_usdt").get<double>();
        const double locked = cfd.at("locked_margin_usdt").get<double>();
        const double realized = cfd.at("realized_pnl_usdt").get<double>();
        const double fees = cfd.at("fees_paid_usdt").get<double>();

        // The identity the audit requires per epoch. Tolerance 1e-6 is far tighter than
        // the 0.005 acceptance rule; the snapshots are floating-point sums and the
        // residuals here are ~1e-13.
        const double leftHandSide = cash + locked;
        const double rightHandSide = snapshot.seedUsdt + realized - fees;
        EXPECT_NEAR(rightHandSide, leftHandSide, 1e-6)
            << "cash " << cash << " + locked " << locked << " != seed " << snapshot.seedUsdt << " + realized "
            << realized << " - fees " << fees;
    }
}

TEST(ReconciliationContract, SnapshotValuesMatchTheAuditTable)
{
    // These are the exact numbers from audit §2. Pinning them is the point: a future
    // "fix" that quietly regenerates the fixtures from post-fix data must fail here.
    for (const auto &snapshot : kSnapshots)
    {
        SCOPED_TRACE(snapshot.epochId);
        const nlohmann::json state = loadJson(kFixtureDir / snapshot.file);
        const nlohmann::json &cfd = state["account"]["cfd"];

        EXPECT_NEAR(snapshot.cashUsdt, cfd.at("cash_usdt").get<double>(), 1e-6);
        EXPECT_NEAR(snapshot.lockedMarginUsdt, cfd.at("locked_margin_usdt").get<double>(), 1e-6);
        EXPECT_NEAR(snapshot.realizedPnlUsdt, cfd.at("realized_pnl_usdt").get<double>(), 1e-6);
        EXPECT_NEAR(snapshot.feesPaidUsdt, cfd.at("fees_paid_usdt").get<double>(), 1e-6);

        // The account id is part of the evidence: the audit's account identity is for
        // "paper-default" only.
        EXPECT_EQ(std::string("paper-default"), state["account"].at("account_id").get<std::string>());
    }
}

#ifdef PULSE_ENABLE_SQLITE
// ---------------------------------------------------------------------------
// 3. The frozen trade table reproduces the audit's per-epoch sums exactly.
// ---------------------------------------------------------------------------
TEST(ReconciliationContract, TradeFixtureReproducesEpochSums)
{
    const std::filesystem::path dbPath = kFixtureDir / "trades-paper-epochs.db";

    // 1. Open read-only: the fixture is evidence, never scratch space. No `immutable=1`
    //    either — that flag is banned for live WAL databases and this test must model
    //    the same discipline on the copy.
    SQLite::Database db(dbPath.string(), SQLite::OPEN_READONLY);

    // 2. The fixture pins the pre-fix schema (user_version 1). P1/P5 will migrate the
    //    live schema; the evidence must stay at the version the audit read.
    EXPECT_EQ(1, db.execAndGet("PRAGMA user_version").getInt());

    // 3. Walk every row and classify by timestamp through the frozen helper — not by
    //    id, so the test proves the boundaries and the ids agree.
    std::array<EpochAccumulator, 4> accumulators{};
    std::set<std::int64_t> ids;
    std::set<std::string> orderIds;
    std::int64_t totalRows = 0;
    double totalPnl = 0.0;
    double totalFees = 0.0;

    SQLite::Statement query(db, "SELECT id, timestamp, pnl, fees, order_id FROM trades ORDER BY id");
    while (query.executeStep())
    {
        const std::int64_t id = query.getColumn(0).getInt64();
        const std::int64_t timestamp = query.getColumn(1).getInt64();
        const double pnl = query.getColumn(2).getDouble();
        const double fees = query.getColumn(3).getDouble();
        const std::string orderId = query.getColumn(4).getString();

        // `trades.timestamp` is UNIX nanoseconds (audit §8). A seconds-scale value
        // would classify into no epoch and silently empty the sums.
        EXPECT_GT(timestamp, 1'700'000'000'000'000'000LL) << "row " << id << " timestamp is not nanoseconds";

        const int epochIndex = classifyEpoch(timestamp);
        ASSERT_GE(epochIndex, 0) << "row " << id << " falls outside every frozen epoch";
        EpochAccumulator &accumulator = accumulators[static_cast<std::size_t>(epochIndex)];

        const bool firstRowInEpoch = (0 == accumulator.rows);
        ++accumulator.rows;
        accumulator.pnl += pnl;
        accumulator.fees += fees;
        accumulator.minId = firstRowInEpoch ? id : (id < accumulator.minId ? id : accumulator.minId);
        accumulator.maxId = firstRowInEpoch ? id : (id > accumulator.maxId ? id : accumulator.maxId);

        ++totalRows;
        totalPnl += pnl;
        totalFees += fees;
        ids.insert(id);
        orderIds.insert(orderId);
    }

    // 4. Per-epoch identity: row count, id span, PnL and fees must match §2 exactly.
    for (std::size_t index = 0; index < kEpochs.size(); ++index)
    {
        const Epoch &epoch = kEpochs[index];
        const EpochAccumulator &accumulator = accumulators[index];
        SCOPED_TRACE(epoch.id);

        EXPECT_EQ(epoch.expectedRows, accumulator.rows);
        EXPECT_EQ(epoch.firstTradeId, accumulator.minId);
        EXPECT_EQ(epoch.lastTradeId, accumulator.maxId);
        EXPECT_NEAR(epoch.expectedPnl, accumulator.pnl, 1e-6);
        EXPECT_NEAR(epoch.expectedFees, accumulator.fees, 1e-6);
    }

    // 5. Lifetime totals and the two known id-integrity facts: id 64 is absent, and
    //    order_id stays unique even though client_order_id does not.
    EXPECT_EQ(210, totalRows);
    EXPECT_NEAR(-99.22, totalPnl, 1e-6);
    EXPECT_NEAR(6.18, totalFees, 1e-6);
    EXPECT_EQ(1, *ids.begin());
    EXPECT_EQ(211, *ids.rbegin());
    EXPECT_EQ(210u, ids.size());
    EXPECT_EQ(0u, ids.count(64));
    EXPECT_EQ(210u, orderIds.size());

    // 6. sqlite_sequence keeps the AUTOINCREMENT high-water mark, so the fixture also
    //    preserves the fact that id 211 was the last id issued at snapshot time.
    EXPECT_EQ(211, db.execAndGet("SELECT COALESCE((SELECT seq FROM sqlite_sequence WHERE name = 'trades'), -1)")
                       .getInt());
}

TEST(ReconciliationContract, FixtureStaysReadOnly)
{
    const std::filesystem::path dbPath = kFixtureDir / "trades-paper-epochs.db";
    const std::filesystem::path walPath = std::filesystem::path(dbPath.string() + "-wal");
    const std::filesystem::path shmPath = std::filesystem::path(dbPath.string() + "-shm");

    // 1. The committed fixture must not carry WAL side files...
    EXPECT_FALSE(std::filesystem::exists(walPath));
    EXPECT_FALSE(std::filesystem::exists(shmPath));

    // 2. ...and a read-only open must not create them. This mirrors the audit rule
    //    that a live WAL database is never opened with immutable=1.
    {
        SQLite::Database db(dbPath.string(), SQLite::OPEN_READONLY);
        EXPECT_EQ(210, db.execAndGet("SELECT COUNT(*) FROM trades").getInt());
    }
    EXPECT_FALSE(std::filesystem::exists(walPath));
    EXPECT_FALSE(std::filesystem::exists(shmPath));
}
#else
TEST(ReconciliationContract, TradeFixtureRequiresSqliteBuild)
{
    GTEST_SKIP() << "PULSE_ENABLE_SQLITE is OFF; the fixture owner must run the SQLite job (P5).";
}
#endif
