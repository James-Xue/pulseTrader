// test_sim_account_store.cpp — unit tests for the simulated-account store.
//
// The store writes data/sim_account.json under the process working directory
// (the test tree here), so each test cleans the file in SetUp/TearDown to
// stay hermetic.

#include "SimAccountStore.hpp"

#include <QFile>

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

class SimAccountStoreTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        QFile::remove(SimAccountStore::filePath());
    }

    void TearDown() override
    {
        QFile::remove(SimAccountStore::filePath());
    }
};

} // namespace

TEST_F(SimAccountStoreTest, SaveRoundTrip)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 10000.0));
    const auto account = SimAccountStore::load(QStringLiteral("keyA"));
    ASSERT_TRUE(account.has_value());
    EXPECT_DOUBLE_EQ(10000.0, account->balanceUsdt);
    EXPECT_FALSE(account->updatedAt.isEmpty());
}

TEST_F(SimAccountStoreTest, LoadMissingFileReturnsNullopt)
{
    EXPECT_FALSE(SimAccountStore::load(QStringLiteral("keyA")).has_value());
}

TEST_F(SimAccountStoreTest, LoadUnknownKeyReturnsNullopt)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 1.0));
    EXPECT_FALSE(SimAccountStore::load(QStringLiteral("keyB")).has_value());
}

TEST_F(SimAccountStoreTest, MultiKeyIsolation)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 111.0));
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyB"), 222.0));
    const auto a = SimAccountStore::load(QStringLiteral("keyA"));
    const auto b = SimAccountStore::load(QStringLiteral("keyB"));
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_DOUBLE_EQ(111.0, a->balanceUsdt);
    EXPECT_DOUBLE_EQ(222.0, b->balanceUsdt);
}

TEST_F(SimAccountStoreTest, OverwriteExisting)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 100.0));
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 50.25));
    EXPECT_DOUBLE_EQ(50.25, SimAccountStore::load(QStringLiteral("keyA"))->balanceUsdt);
}

TEST_F(SimAccountStoreTest, LoadRejectsGarbageFile)
{
    QFile file(SimAccountStore::filePath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("not json at all {{{");
    file.close();
    EXPECT_FALSE(SimAccountStore::load(QStringLiteral("keyA")).has_value());
}

TEST_F(SimAccountStoreTest, EmptyKeyRejected)
{
    EXPECT_FALSE(SimAccountStore::save(QString(), 1.0));
    EXPECT_FALSE(SimAccountStore::load(QString()).has_value());
}

// ---------------------------------------------------------------------------
// debit — paper-trade balance deduction
// ---------------------------------------------------------------------------

TEST_F(SimAccountStoreTest, DebitDeductsAndKeepsRemainder)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 1000.0));
    const auto result = SimAccountStore::debit(QStringLiteral("keyA"), 400.0);
    ASSERT_TRUE(result.ok);
    EXPECT_DOUBLE_EQ(600.0, result.remainingUsdt);
    EXPECT_DOUBLE_EQ(600.0, SimAccountStore::load(QStringLiteral("keyA"))->balanceUsdt);
}

TEST_F(SimAccountStoreTest, DebitFailsWithoutAccount)
{
    const auto result = SimAccountStore::debit(QStringLiteral("keyA"), 10.0);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("未设置模拟账户")));
}

TEST_F(SimAccountStoreTest, DebitFailsOnInsufficientBalance)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 100.0));
    const auto result = SimAccountStore::debit(QStringLiteral("keyA"), 150.0);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.error.contains(QStringLiteral("余额不足")));
    // The store must be untouched on failure.
    EXPECT_DOUBLE_EQ(100.0, SimAccountStore::load(QStringLiteral("keyA"))->balanceUsdt);
}

TEST_F(SimAccountStoreTest, DebitRejectsNonPositiveAmount)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 100.0));
    EXPECT_FALSE(SimAccountStore::debit(QStringLiteral("keyA"), 0.0).ok);
    EXPECT_FALSE(SimAccountStore::debit(QStringLiteral("keyA"), -5.0).ok);
    EXPECT_DOUBLE_EQ(100.0, SimAccountStore::load(QStringLiteral("keyA"))->balanceUsdt);
}

TEST_F(SimAccountStoreTest, DebitExactBalanceLeavesZero)
{
    EXPECT_TRUE(SimAccountStore::save(QStringLiteral("keyA"), 100.0));
    const auto result = SimAccountStore::debit(QStringLiteral("keyA"), 100.0);
    ASSERT_TRUE(result.ok);
    EXPECT_DOUBLE_EQ(0.0, result.remainingUsdt);
}
