#include <QtTest>

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include "engine/EngineLauncher.hpp"
#include "security/SecretStore.hpp"

using namespace pulse::client;

/// Tests the vault cryptography and the engine config generator. No network,
/// no engine process, no Qt GUI: the vault is exercised on a temporary file.
class TestSecretStore : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip();
    void wrongPasswordFails();
    void saveWithCachedKey();
    void corruptVaultFails();
    void fileIsOwnerOnly();
    void configMainnet();
    void configTestnetForcesFutures();
    void configEscapesSecret();

private:
    static QJsonObject samplePayload();
};

QJsonObject TestSecretStore::samplePayload()
{
    QJsonObject profile;
    profile[QStringLiteral("network")] = QStringLiteral("mainnet");
    profile[QStringLiteral("market")] = QStringLiteral("spot");
    profile[QStringLiteral("apiKey")] = QStringLiteral("key-123");
    profile[QStringLiteral("apiSecret")] = QStringLiteral("secret-456");
    profile[QStringLiteral("proxy")] = QString();

    QJsonObject payload;
    payload[QStringLiteral("username")] = QStringLiteral("james");
    payload[QStringLiteral("profile")] = profile;
    return payload;
}

void TestSecretStore::roundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vault.bin"));

    QByteArray key;
    QString error;
    QVERIFY2(SecretStore::create(path, samplePayload(), QStringLiteral("correct horse"),
                                 key, &error), qPrintable(error));
    QVERIFY(SecretStore::exists(path));

    QByteArray unlockedKey;
    const auto payload = SecretStore::unlock(path, QStringLiteral("correct horse"),
                                             unlockedKey, &error);
    QVERIFY2(payload.has_value(), qPrintable(error));
    QCOMPARE(payload->value(QStringLiteral("username")).toString(),
             QStringLiteral("james"));
    QCOMPARE(payload->value(QStringLiteral("profile")).toObject()
                 .value(QStringLiteral("apiKey")).toString(),
             QStringLiteral("key-123"));

    // The key handed back must be the same material used to encrypt.
    QCOMPARE(unlockedKey.size(), SecretStore::kKeyBytes);
    QCOMPARE(unlockedKey, key);
}

void TestSecretStore::wrongPasswordFails()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("vault.bin"));

    QByteArray key;
    QString error;
    QVERIFY(SecretStore::create(path, samplePayload(), QStringLiteral("correct horse"),
                                key, &error));

    QByteArray wrongKey;
    const auto payload = SecretStore::unlock(path, QStringLiteral("wrong password"),
                                             wrongKey, &error);
    QVERIFY(!payload.has_value());
    QVERIFY(error.contains(QStringLiteral("wrong password")));
}

void TestSecretStore::saveWithCachedKey()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("vault.bin"));

    QByteArray key;
    QString error;
    QVERIFY(SecretStore::create(path, samplePayload(), QStringLiteral("correct horse"),
                                key, &error));

    // Change the profile and re-encrypt without the password (the account
    // service keeps the derived key while unlocked).
    QJsonObject updated = samplePayload();
    QJsonObject profile = updated.value(QStringLiteral("profile")).toObject();
    profile[QStringLiteral("apiKey")] = QStringLiteral("rotated-key");
    updated[QStringLiteral("profile")] = profile;

    QVERIFY2(SecretStore::save(path, updated, key, &error), qPrintable(error));

    QByteArray reopenedKey;
    const auto payload = SecretStore::unlock(path, QStringLiteral("correct horse"),
                                             reopenedKey, &error);
    QVERIFY2(payload.has_value(), qPrintable(error));
    QCOMPARE(payload->value(QStringLiteral("profile")).toObject()
                 .value(QStringLiteral("apiKey")).toString(),
             QStringLiteral("rotated-key"));
}

void TestSecretStore::corruptVaultFails()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("vault.bin"));

    QByteArray key;
    QString error;
    QVERIFY(SecretStore::create(path, samplePayload(), QStringLiteral("correct horse"),
                                key, &error));

    // Flip one byte inside the ciphertext: the GCM tag must reject it.
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QByteArray blob = file.readAll();
    QVERIFY(blob.size() > 60);
    blob[blob.size() - 1] = static_cast<char>(blob.at(blob.size() - 1) ^ 0x01);
    file.seek(0);
    QVERIFY(file.write(blob) == blob.size());
    file.close();

    QByteArray reopenedKey;
    const auto payload = SecretStore::unlock(path, QStringLiteral("correct horse"),
                                             reopenedKey, &error);
    QVERIFY(!payload.has_value());
}

void TestSecretStore::fileIsOwnerOnly()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("vault.bin"));

    QByteArray key;
    QString error;
    QVERIFY(SecretStore::create(path, samplePayload(), QStringLiteral("correct horse"),
                                key, &error));

    const QFile::Permissions permissions = QFile::permissions(path);
    QVERIFY(permissions.testFlag(QFile::ReadOwner));
    QVERIFY(permissions.testFlag(QFile::WriteOwner));
    QVERIFY(!permissions.testFlag(QFile::ReadGroup));
    QVERIFY(!permissions.testFlag(QFile::ReadOther));
}

void TestSecretStore::configMainnet()
{
    const QString dir = QStringLiteral("/run/user/1000/pulseTrader");
    const QString config = EngineLauncher::buildConfig(
        QStringLiteral("KEY"), QStringLiteral("SECRET"),
        QStringLiteral("mainnet"), QStringLiteral("spot"),
        QStringLiteral("http://127.0.0.1:7897"), 8081, dir);

    QVERIFY(config.contains(QStringLiteral("testnet = false")));
    QVERIFY(config.contains(QStringLiteral("active_market = \"spot\"")));
    QVERIFY(config.contains(QStringLiteral("market_type = \"spot\"")));
    QVERIFY(config.contains(QStringLiteral("signal_only = true")));
    QVERIFY(config.contains(QStringLiteral("proxyUrl = \"http://127.0.0.1:7897\"")));
    QVERIFY(config.contains(QStringLiteral("port = 8081")));
    QVERIFY(config.contains(dir + QStringLiteral("/trades.db")));
}

void TestSecretStore::configTestnetForcesFutures()
{
    // Gate.io testnet is futures-only; a testnet profile that selected spot
    // must still produce a config the validator accepts.
    const QString config = EngineLauncher::buildConfig(
        QStringLiteral("KEY"), QStringLiteral("SECRET"),
        QStringLiteral("testnet"), QStringLiteral("spot"),
        QString(), 8081, QStringLiteral("/tmp/rt"));

    QVERIFY(config.contains(QStringLiteral("testnet = true")));
    QVERIFY(config.contains(QStringLiteral("active_market = \"futures\"")));
    QVERIFY(!config.contains(QStringLiteral("market_type = \"spot\"")));
    QVERIFY(config.contains(QStringLiteral("market_type = \"futures\"")));
    QVERIFY(!config.contains(QStringLiteral("proxyUrl")));
}

void TestSecretStore::configEscapesSecret()
{
    const QString config = EngineLauncher::buildConfig(
        QStringLiteral("KEY"), QStringLiteral("sec\"ret\\x"),
        QStringLiteral("mainnet"), QStringLiteral("futures"),
        QString(), 8081, QStringLiteral("/tmp/rt"));

    QVERIFY(config.contains(QStringLiteral("apiSecret = \"sec\\\"ret\\\\x\"")));
}

QTEST_MAIN(TestSecretStore)

#include "tst_secretstore.moc"
