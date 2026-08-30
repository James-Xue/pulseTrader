// test_credentials_store.cpp — unit tests for the .env credential store.
//
// All tests use QTemporaryDir so they never touch the real project .env.

#include "CredentialsStore.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <gtest/gtest.h>

using namespace pulse::ui;

namespace
{

// Write a raw .env file and return its path.
QString writeEnv(QTemporaryDir &dir, const QString &content)
{
    const QString path = dir.filePath(QStringLiteral("env_test"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return {};
    }
    QTextStream out(&file);
    out << content;
    return path;
}

} // namespace

// ---------------------------------------------------------------------------
// maskKey
// ---------------------------------------------------------------------------

TEST(CredentialsStoreTest, MaskKeyShortReturnsVerbatim)
{
    // Keys of 8 chars or fewer carry no meaningful prefix/suffix — show all.
    EXPECT_EQ(QStringLiteral("abcdefgh"), CredentialsStore::maskKey(QStringLiteral("abcdefgh")));
    EXPECT_EQ(QStringLiteral(""), CredentialsStore::maskKey(QStringLiteral("")));
}

TEST(CredentialsStoreTest, MaskKeyLongShowsFirstAndLastFour)
{
    // Account identity: ab12…cd34 — never the full key.
    EXPECT_EQ(QStringLiteral("ab12…cd34"),
              CredentialsStore::maskKey(QStringLiteral("ab12XYZWcd34")));
}

// ---------------------------------------------------------------------------
// loadFromEnvFile
// ---------------------------------------------------------------------------

TEST(CredentialsStoreTest, LoadPrefersMainnetNames)
{
    // When both canonical and legacy names exist, GATE_MAINNET_* wins.
    QTemporaryDir dir;
    const QString path = writeEnv(dir,
        QStringLiteral("GATE_API_KEY=legacy_key\n"
                       "GATE_MAINNET_API_KEY=mainnet_key\n"
                       "GATE_MAINNET_API_SECRET=mainnet_secret\n"));
    const StoredCredentials creds = CredentialsStore::loadFromEnvFile(path);
    EXPECT_EQ(QStringLiteral("mainnet_key"), creds.apiKey);
    EXPECT_EQ(QStringLiteral("mainnet_secret"), creds.apiSecret);
}

TEST(CredentialsStoreTest, LoadFallsBackToLegacyNames)
{
    QTemporaryDir dir;
    const QString path = writeEnv(dir,
        QStringLiteral("GATE_API_KEY=legacy_key\nGATE_API_SECRET=legacy_secret\n"));
    const StoredCredentials creds = CredentialsStore::loadFromEnvFile(path);
    EXPECT_EQ(QStringLiteral("legacy_key"), creds.apiKey);
    EXPECT_EQ(QStringLiteral("legacy_secret"), creds.apiSecret);
}

TEST(CredentialsStoreTest, LoadSkipsCommentsBlanksAndQuotes)
{
    // '#' lines and blanks are ignored; values may be quoted (bash style).
    QTemporaryDir dir;
    const QString path = writeEnv(dir,
        QStringLiteral("# comment line\n"
                       "\n"
                       "GATE_MAINNET_API_KEY=\"quoted key\"\n"
                       "PULSE_NETWORK=testnet\n"
                       "GATE_MAINNET_API_SECRET='secret'\n"));
    const StoredCredentials creds = CredentialsStore::loadFromEnvFile(path);
    EXPECT_EQ(QStringLiteral("quoted key"), creds.apiKey);
    EXPECT_EQ(QStringLiteral("secret"), creds.apiSecret);
}

TEST(CredentialsStoreTest, LoadMissingFileReturnsEmpty)
{
    QTemporaryDir dir;
    const StoredCredentials creds =
        CredentialsStore::loadFromEnvFile(dir.filePath(QStringLiteral("nope")));
    EXPECT_TRUE(creds.apiKey.isEmpty());
    EXPECT_TRUE(creds.apiSecret.isEmpty());
}

// ---------------------------------------------------------------------------
// loadAllEnvVars
// ---------------------------------------------------------------------------

TEST(CredentialsStoreTest, LoadAllVarsKeepsEveryPair)
{
    QTemporaryDir dir;
    const QString path = writeEnv(dir,
        QStringLiteral("PULSE_NETWORK=mainnet\n"
                       "# a comment\n"
                       "AI_API_KEY=sk-abc\n"
                       "GATE_MAINNET_API_KEY=k\nGATE_MAINNET_API_SECRET=s\n"));
    const QMap<QString, QString> vars = CredentialsStore::loadAllEnvVars(path);
    EXPECT_EQ(4, vars.size());
    EXPECT_EQ(QStringLiteral("mainnet"), vars.value(QStringLiteral("PULSE_NETWORK")));
    EXPECT_EQ(QStringLiteral("sk-abc"), vars.value(QStringLiteral("AI_API_KEY")));
    EXPECT_EQ(QStringLiteral("k"), vars.value(QStringLiteral("GATE_MAINNET_API_KEY")));
}

// ---------------------------------------------------------------------------
// saveToEnvFile
// ---------------------------------------------------------------------------

TEST(CredentialsStoreTest, SaveCreatesNewFile)
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("env_new"));
    EXPECT_TRUE(CredentialsStore::saveToEnvFile(path, QStringLiteral("key1"), QStringLiteral("sec1")));

    const StoredCredentials creds = CredentialsStore::loadFromEnvFile(path);
    EXPECT_EQ(QStringLiteral("key1"), creds.apiKey);
    EXPECT_EQ(QStringLiteral("sec1"), creds.apiSecret);
}

TEST(CredentialsStoreTest, SavePreservesUnrelatedLines)
{
    // PULSE_NETWORK / AI_API_KEY must survive a credential rewrite.
    QTemporaryDir dir;
    const QString path = writeEnv(dir,
        QStringLiteral("PULSE_NETWORK=testnet\n"
                       "AI_API_KEY=sk-old\n"
                       "GATE_MAINNET_API_KEY=old_key\n"
                       "GATE_MAINNET_API_SECRET=old_secret\n"));
    EXPECT_TRUE(CredentialsStore::saveToEnvFile(path, QStringLiteral("new_key"), QStringLiteral("new_secret")));

    const QMap<QString, QString> vars = CredentialsStore::loadAllEnvVars(path);
    EXPECT_EQ(QStringLiteral("testnet"), vars.value(QStringLiteral("PULSE_NETWORK")));
    EXPECT_EQ(QStringLiteral("sk-old"), vars.value(QStringLiteral("AI_API_KEY")));
    EXPECT_EQ(QStringLiteral("new_key"), vars.value(QStringLiteral("GATE_MAINNET_API_KEY")));
    EXPECT_EQ(QStringLiteral("new_secret"), vars.value(QStringLiteral("GATE_MAINNET_API_SECRET")));
}

TEST(CredentialsStoreTest, SaveAlsoUpdatesLegacyNames)
{
    // Legacy GATE_API_* lines (used by old launcher scripts) stay in sync.
    QTemporaryDir dir;
    const QString path = writeEnv(dir,
        QStringLiteral("GATE_API_KEY=old_legacy\nGATE_API_SECRET=old_legacy_sec\n"));
    EXPECT_TRUE(CredentialsStore::saveToEnvFile(path, QStringLiteral("new_key"), QStringLiteral("new_secret")));

    const QMap<QString, QString> vars = CredentialsStore::loadAllEnvVars(path);
    EXPECT_EQ(QStringLiteral("new_key"), vars.value(QStringLiteral("GATE_API_KEY")));
    EXPECT_EQ(QStringLiteral("new_key"), vars.value(QStringLiteral("GATE_MAINNET_API_KEY")));
    EXPECT_EQ(QStringLiteral("new_secret"), vars.value(QStringLiteral("GATE_API_SECRET")));
}

TEST(CredentialsStoreTest, SaveRoundTrip)
{
    // Save twice — the second save must not corrupt or duplicate lines.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("env_rt"));
    EXPECT_TRUE(CredentialsStore::saveToEnvFile(path, QStringLiteral("a"), QStringLiteral("b")));
    EXPECT_TRUE(CredentialsStore::saveToEnvFile(path, QStringLiteral("c"), QStringLiteral("d")));

    const QMap<QString, QString> vars = CredentialsStore::loadAllEnvVars(path);
    EXPECT_EQ(QStringLiteral("c"), vars.value(QStringLiteral("GATE_MAINNET_API_KEY")));
    EXPECT_EQ(QStringLiteral("d"), vars.value(QStringLiteral("GATE_MAINNET_API_SECRET")));
}
