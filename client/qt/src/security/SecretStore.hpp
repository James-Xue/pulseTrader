#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace pulse::client
{

/// Encrypted vault for the local account and its exchange profile.
///
/// File layout (all integers big-endian):
///
///   "PTPV" | version u8 | iterations u32 | salt 16 | iv 12 | tag 16 | ct
///
/// Security properties:
/// 1. The key is derived from the local password with PBKDF2-HMAC-SHA256 and a
///    per-vault random salt. The password itself is never stored.
/// 2. Payload encryption is AES-256-GCM. A wrong password fails the GCM tag
///    check, so no separate password hash exists to be attacked offline.
/// 3. Every save uses a fresh IV; reusing an IV under the same key would
///    destroy GCM's confidentiality guarantee.
///
/// Thread affinity: callers must use one thread (the GUI thread). OpenSSL's
/// EVP API is thread-safe across contexts, but the vault file is not locked.
class SecretStore
{
public:
    /// PBKDF2 work factor. 600k SHA-256 iterations is the OWASP 2023 floor for
    /// password-derived keys; a laptop pays roughly 0.3–0.6 s at unlock.
    static constexpr int kIterations = 600'000;
    static constexpr int kSaltBytes = 16;
    static constexpr int kIvBytes = 12;
    static constexpr int kTagBytes = 16;
    static constexpr int kKeyBytes = 32;

    /// True when a vault file exists (an account has been created before).
    [[nodiscard]] static bool exists(const QString& path);

    /// Create a new vault. Generates the salt and IV, derives the key from the
    /// password, and writes the encrypted payload. `keyOut` receives the
    /// derived key so the caller can re-save without re-entering the password.
    static bool create(const QString& path, const QJsonObject& payload,
                       const QString& password, QByteArray& keyOut,
                       QString* error);

    /// Decrypt an existing vault. Returns an empty optional on a wrong
    /// password, a corrupt file, or an I/O error; `error` explains which.
    /// On success `keyOut` receives the derived key so the caller can save
    /// profile updates without paying for a second PBKDF2 derivation.
    [[nodiscard]] static std::optional<QJsonObject> unlock(
        const QString& path, const QString& password, QByteArray& keyOut,
        QString* error);

    /// Re-encrypt the payload with the cached key and a fresh IV.
    static bool save(const QString& path, const QJsonObject& payload,
                     const QByteArray& key, QString* error);

    /// Best-effort wipe of key material. OpenSSL's cleanse is not optimised
    /// away, unlike a plain fill().
    static void wipe(QByteArray& secret);
};

} // namespace pulse::client
