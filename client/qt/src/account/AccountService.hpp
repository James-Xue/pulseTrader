#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace pulse::client
{

/// Local account and exchange-profile state for the Qt client.
///
/// The vault is a single encrypted file under the application config
/// directory. It holds one local account and the Gate.io profile belonging to
/// it. The password unlocks the vault; there is no separate password hash, so
/// a wrong password and a tampered file are indistinguishable (both fail the
/// AES-GCM tag check).
///
/// Thread affinity: GUI thread only. The derived key lives in memory only
/// while the account is unlocked and is wiped on sign-out.
class AccountService : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool hasAccount READ hasAccount NOTIFY stateChanged)
    Q_PROPERTY(bool unlocked READ unlocked NOTIFY stateChanged)
    Q_PROPERTY(QString username READ username NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    // Profile fields, populated after unlock and after every save. QML uses
    // them only to prefill the profile form.
    Q_PROPERTY(QString profileNetwork READ profileNetwork NOTIFY profileChanged)
    Q_PROPERTY(QString profileMarket READ profileMarket NOTIFY profileChanged)
    Q_PROPERTY(QString profileApiKey READ profileApiKey NOTIFY profileChanged)
    Q_PROPERTY(QString profileApiSecret READ profileApiSecret NOTIFY profileChanged)
    Q_PROPERTY(QString profileProxy READ profileProxy NOTIFY profileChanged)
    /// Active-market watchlist as a comma-separated string, e.g.
    /// "BTC_USDT, ETH_USDT, XAU_USDT".
    Q_PROPERTY(QString profileSymbols READ profileSymbols NOTIFY profileChanged)

public:
    explicit AccountService(QObject* parent = nullptr);
    ~AccountService() override;

    /// Create the local account. Refuses when a vault already exists or the
    /// two password fields differ.
    Q_INVOKABLE void createAccount(const QString& username,
                                   const QString& password,
                                   const QString& confirm);

    /// Unlock the vault. The decrypted username must match the one typed.
    Q_INVOKABLE void unlock(const QString& username, const QString& password);

    /// Wipe the derived key and every cached secret, returning to the login
    /// screen. The vault file itself is untouched.
    Q_INVOKABLE void signOut();

    /// Validate and persist the Gate.io profile. Requires an unlocked vault.
    Q_INVOKABLE bool saveProfile(const QString& network,
                                 const QString& market,
                                 const QString& apiKey,
                                 const QString& apiSecret,
                                 const QString& proxy,
                                 const QString& symbols);

    [[nodiscard]] bool hasAccount() const { return m_hasAccount; }
    [[nodiscard]] bool unlocked() const { return m_unlocked; }
    [[nodiscard]] QString username() const { return m_username; }
    [[nodiscard]] QString lastError() const { return m_lastError; }
    [[nodiscard]] QString profileNetwork() const;
    [[nodiscard]] QString profileMarket() const;
    [[nodiscard]] QString profileApiKey() const;
    [[nodiscard]] QString profileApiSecret() const;
    [[nodiscard]] QString profileProxy() const;
    [[nodiscard]] QString profileSymbols() const;

signals:
    void stateChanged();
    void profileChanged();
    void lastErrorChanged();

private:
    /// Path of the encrypted vault, created on first use.
    [[nodiscard]] static QString vaultPath();

    void setError(const QString& message);
    void clearError();
    void refreshProfile();

    QByteArray m_key;                 ///< Derived key, valid only when unlocked.
    QJsonObject m_payload;            ///< Decrypted vault contents.
    bool m_hasAccount = false;
    bool m_unlocked = false;
    QString m_username;
    QString m_lastError;
};

} // namespace pulse::client
