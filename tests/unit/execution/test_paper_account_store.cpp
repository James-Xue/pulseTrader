// test_paper_account_store.cpp — Crash-safe ledger persistence.
//
// Every test uses its own directory under the system temp dir, removed before
// and after the case, so the suite is order-independent and leaves no state.

#include "execution/PaperAccountStore.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace pulse;
using namespace pulse::execution;

namespace
{

PaperConfig makePaperConfig()
{
    PaperConfig cfg;
    cfg.account_id = "store-test";
    cfg.initial_balance_usdt = 5'000.0;
    return cfg;
}

/// Create a clean directory for one test case.
std::filesystem::path freshDir(const std::string &name)
{
    const auto dir = std::filesystem::temp_directory_path() / ("pulse-paper-" + name);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void writeText(const std::filesystem::path &p, const std::string &text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST(PaperAccountStoreTest, FirstRunWhenNoFileExists)
{
    const auto dir = freshDir("first-run");
    PaperAccountStore store(dir / "state.json", 1'000);
    PaperAccount account(makePaperConfig());

    EXPECT_EQ(PaperStoreLoad::FirstRun, store.load(account));
    EXPECT_DOUBLE_EQ(15'000.0, account.snapshot(0.0).equity_usdt);
}

TEST(PaperAccountStoreTest, SaveThenLoadRestoresLedger)
{
    const auto dir = freshDir("save-load");
    PaperAccount account(makePaperConfig());
    ASSERT_TRUE(account.lockMargin(PaperLeg::Cfd, 1'500.0));
    account.chargeFee(PaperLeg::Cfd, 6.0);
    account.updateEquityPeak(15'250.0);

    {
        PaperAccountStore store(dir / "state.json", 1'000);
        store.saveNow(account);
    }

    // No .tmp file may survive a successful publish.
    EXPECT_FALSE(std::filesystem::exists(dir / "state.json.tmp"));

    PaperAccount reloaded(makePaperConfig());
    PaperAccountStore store(dir / "state.json", 1'000);
    EXPECT_EQ(PaperStoreLoad::Loaded, store.load(reloaded));

    const auto before = account.snapshot(0.0);
    const auto after = reloaded.snapshot(0.0);
    EXPECT_DOUBLE_EQ(before.equity_usdt, after.equity_usdt);
    EXPECT_DOUBLE_EQ(before.cfd.cash_usdt, after.cfd.cash_usdt);
    EXPECT_DOUBLE_EQ(before.cfd.locked_margin_usdt,
                     after.cfd.locked_margin_usdt);
    EXPECT_DOUBLE_EQ(15'250.0, after.peak_equity_usdt);
}

TEST(PaperAccountStoreTest, CorruptMainFallsBackToBackup)
{
    const auto dir = freshDir("bak-fallback");
    const auto path = dir / "state.json";

    PaperAccount account(makePaperConfig());
    ASSERT_TRUE(account.lockMargin(PaperLeg::Futures, 2'000.0));

    PaperAccountStore store(path, 1'000);
    store.saveNow(account); // first publish: no backup yet
    store.saveNow(account); // second publish rotates the first into .bak

    ASSERT_TRUE(std::filesystem::exists(path));
    ASSERT_TRUE(std::filesystem::exists(path.string() + ".bak"));

    writeText(path, "{ this is not json");

    PaperAccount reloaded(makePaperConfig());
    EXPECT_EQ(PaperStoreLoad::Loaded, store.load(reloaded));
    EXPECT_DOUBLE_EQ(2'000.0, reloaded.snapshot(0.0).futures.locked_margin_usdt);
}

TEST(PaperAccountStoreTest, BothCopiesCorruptIsFatal)
{
    const auto dir = freshDir("both-corrupt");
    const auto path = dir / "state.json";

    writeText(path, "garbage");
    writeText(path.string() + ".bak", "also garbage");

    PaperAccountStore store(path, 1'000);
    PaperAccount account(makePaperConfig());
    EXPECT_EQ(PaperStoreLoad::Corrupt, store.load(account));
}

TEST(PaperAccountStoreTest, WrongSchemaVersionIsCorrupt)
{
    const auto dir = freshDir("schema");
    const auto path = dir / "state.json";
    writeText(path, R"({"schema_version": 999, "account": {}})");

    PaperAccountStore store(path, 1'000);
    PaperAccount account(makePaperConfig());
    EXPECT_EQ(PaperStoreLoad::Corrupt, store.load(account));
}

TEST(PaperAccountStoreTest, WriterThreadPersistsWithoutManualSave)
{
    const auto dir = freshDir("writer");
    const auto path = dir / "state.json";

    PaperAccount account(makePaperConfig());
    {
        // 100 ms cadence; wait long enough for at least one tick.
        PaperAccountStore store(path, 100);
        store.start(account);
        for (int i = 0; i < 50 && !std::filesystem::exists(path); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        store.stop();
    }

    ASSERT_TRUE(std::filesystem::exists(path));

    PaperAccount reloaded(makePaperConfig());
    PaperAccountStore reader(path, 1'000);
    EXPECT_EQ(PaperStoreLoad::Loaded, reader.load(reloaded));
}

TEST(PaperAccountStoreTest, VenueSectionRoundTrips)
{
    const auto dir = freshDir("venue-section");
    const auto path = dir / "state.json";
    PaperAccount account(makePaperConfig());

    {
        PaperAccountStore store(path, 1'000);
        store.setVenueProvider(
            []
            {
                return nlohmann::json{
                    { "seq", 7 },
                    { "positions", nlohmann::json::array() },
                };
            });
        store.saveNow(account);
    }

    PaperAccount reloaded(makePaperConfig());
    PaperAccountStore store(path, 1'000);
    EXPECT_EQ(PaperStoreLoad::Loaded, store.load(reloaded));
    ASSERT_TRUE(store.loadedVenue().is_object());
    EXPECT_EQ(7, store.loadedVenue().value("seq", 0));
}

TEST(PaperAccountStoreTest, AcceptsLegacySchemaV1WithoutVenue)
{
    const auto dir = freshDir("schema-v1");
    const auto path = dir / "state.json";
    PaperAccount account(makePaperConfig());

    nlohmann::json legacy;
    legacy["schema_version"] = 1;
    legacy["account"] = account.toJson();
    writeText(path, legacy.dump(2));

    PaperAccount reloaded(makePaperConfig());
    PaperAccountStore store(path, 1'000);
    EXPECT_EQ(PaperStoreLoad::Loaded, store.load(reloaded));
    EXPECT_TRUE(store.loadedVenue().empty());
}

TEST(PaperAccountStoreTest, StateFileCarriesNoCredentials)
{
    const auto dir = freshDir("no-secrets");
    const auto path = dir / "state.json";

    PaperConfig cfg = makePaperConfig();
    cfg.account_id = "paper-no-key";
    PaperAccount account(cfg);
    PaperAccountStore store(path, 1'000);
    store.saveNow(account);

    std::ifstream in(path);
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    EXPECT_EQ(std::string::npos, contents.find("apiKey"));
    EXPECT_EQ(std::string::npos, contents.find("apiSecret"));
    EXPECT_NE(std::string::npos, contents.find("paper-no-key"));
}
