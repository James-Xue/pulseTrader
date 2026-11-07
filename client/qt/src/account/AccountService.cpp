#include "account/AccountService.hpp"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include "security/SecretStore.hpp"

namespace pulse::client
{

namespace
{

constexpr int kMinPasswordLength = 8;
constexpr int kMinUsernameLength = 2;

/// Sections of the profile JSON. Kept as constants so a typo cannot silently
/// read an empty value.
constexpr char kProfileKey[] = "profile";

QString profileString(const QJsonObject& payload, const char* key)
{
    return payload.value(QString::fromLatin1(kProfileKey))
        .toObject()
        .value(QString::fromLatin1(key))
        .toString();
}

} // namespace

AccountService::AccountService(QObject* parent)
    : QObject(parent)
{
    m_hasAccount = SecretStore::exists(vaultPath());
}

AccountService::~AccountService()
{
    SecretStore::wipe(m_key);
}

QString AccountService::vaultPath()
{
    // 1. AppConfigLocation is ~/.config/pulseTrader on Linux, derived from the
    //    application and organisation names set in main.cpp.
    QString directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);

    if (directory.isEmpty())
    {
        directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }

    // 2. Create it 0700 so another local user cannot even list the vault.
    QDir().mkpath(directory);
    QFile::setPermissions(directory,
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    return directory + QStringLiteral("/vault.bin");
}

void AccountService::createAccount(const QString& username,
                                   const QString& password,
                                   const QString& confirm)
{
    clearError();

    // 1. Refuse to overwrite an existing vault: that would destroy the only
    //    copy of the saved exchange credentials.
    if (SecretStore::exists(vaultPath()))
    {
        setError(tr("An account already exists on this machine."));
        return;
    }

    // 2. Validate the typed values. The username is cosmetic, but it is stored
    //    inside the vault and checked on unlock so two people cannot silently
    //    share one vault.
    const QString accountName = username.trimmed();

    if (accountName.size() < kMinUsernameLength)
    {
        setError(tr("Username must be at least %1 characters.").arg(kMinUsernameLength));
        return;
    }

    if (password.size() < kMinPasswordLength)
    {
        setError(tr("Password must be at least %1 characters.").arg(kMinPasswordLength));
        return;
    }

    if (password != confirm)
    {
        setError(tr("The two passwords do not match."));
        return;
    }

    // 3. Start with a mainnet, spot profile and empty credentials; the profile
    //    screen fills them in before the first engine start.
    QJsonObject profile;
    profile[QStringLiteral("network")] = QStringLiteral("mainnet");
    profile[QStringLiteral("market")] = QStringLiteral("spot");
    profile[QStringLiteral("apiKey")] = QString();
    profile[QStringLiteral("apiSecret")] = QString();
    profile[QStringLiteral("proxy")] = QString();

    QJsonObject payload;
    payload[QStringLiteral("username")] = accountName;
    payload[QStringLiteral("profile")] = profile;

    QString error;

    if (!SecretStore::create(vaultPath(), payload, password, m_key, &error))
    {
        setError(error);
        return;
    }

    m_payload = payload;
    m_username = accountName;
    m_hasAccount = true;
    m_unlocked = true;
    refreshProfile();
    emit stateChanged();
}

void AccountService::unlock(const QString& username, const QString& password)
{
    clearError();

    if (!SecretStore::exists(vaultPath()))
    {
        setError(tr("No local account exists yet."));
        return;
    }

    QString error;
    QByteArray key;
    const auto payload = SecretStore::unlock(vaultPath(), password, key, &error);

    if (!payload.has_value())
    {
        setError(error);
        return;
    }

    // 1. The username is inside the encrypted payload, so a mismatch means the
    //    caller typed another profile's name — the password was still correct.
    const QString storedName = payload->value(QStringLiteral("username")).toString();
    const QString typedName = username.trimmed();

    if (!typedName.isEmpty() && typedName != storedName)
    {
        setError(tr("Wrong account name for this vault."));
        return;
    }

    // 2. Keep the derived key for profile saves. unlock() handed it over
    //    without wiping it, precisely so this costs one PBKDF2 pass.
    m_key = key;
    m_payload = *payload;
    m_username = storedName;
    m_unlocked = true;
    refreshProfile();
    emit stateChanged();
}

void AccountService::signOut()
{
    SecretStore::wipe(m_key);
    m_payload = QJsonObject{};
    m_unlocked = false;
    m_username.clear();
    refreshProfile();
    emit stateChanged();
}

bool AccountService::saveProfile(const QString& network,
                                 const QString& market,
                                 const QString& apiKey,
                                 const QString& apiSecret,
                                 const QString& proxy)
{
    clearError();

    if (!m_unlocked)
    {
        setError(tr("Unlock the local account first."));
        return false;
    }

    // 1. Constrain the selectors to what the engine accepts. The engine
    //    validates too, but failing here gives a precise message.
    if (network != QStringLiteral("mainnet") && network != QStringLiteral("testnet"))
    {
        setError(tr("Network must be mainnet or testnet."));
        return false;
    }

    if (market != QStringLiteral("spot") && market != QStringLiteral("futures")
        && market != QStringLiteral("cfd"))
    {
        setError(tr("Market must be spot, futures or cfd."));
        return false;
    }

    if (apiKey.trimmed().isEmpty() || apiSecret.trimmed().isEmpty())
    {
        setError(tr("API key and secret are required."));
        return false;
    }

    // 2. Update the decrypted payload and re-encrypt it with a fresh IV.
    QJsonObject profile;
    profile[QStringLiteral("network")] = network;
    profile[QStringLiteral("market")] = market;
    profile[QStringLiteral("apiKey")] = apiKey.trimmed();
    profile[QStringLiteral("apiSecret")] = apiSecret.trimmed();
    profile[QStringLiteral("proxy")] = proxy.trimmed();

    m_payload[QStringLiteral("profile")] = profile;

    QString error;

    if (!SecretStore::save(vaultPath(), m_payload, m_key, &error))
    {
        setError(error);
        return false;
    }

    refreshProfile();
    return true;
}

QString AccountService::profileNetwork() const
{
    return profileString(m_payload, "network");
}

QString AccountService::profileMarket() const
{
    return profileString(m_payload, "market");
}

QString AccountService::profileApiKey() const
{
    return profileString(m_payload, "apiKey");
}

QString AccountService::profileApiSecret() const
{
    return profileString(m_payload, "apiSecret");
}

QString AccountService::profileProxy() const
{
    return profileString(m_payload, "proxy");
}

void AccountService::setError(const QString& message)
{
    m_lastError = message;
    emit lastErrorChanged();
}

void AccountService::clearError()
{
    if (!m_lastError.isEmpty())
    {
        m_lastError.clear();
        emit lastErrorChanged();
    }
}

void AccountService::refreshProfile()
{
    emit profileChanged();
}

} // namespace pulse::client
