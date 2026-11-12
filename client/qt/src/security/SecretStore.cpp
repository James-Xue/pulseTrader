#include "security/SecretStore.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtEndian>

#include <cstring>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace pulse::client
{

namespace
{

constexpr char kMagic[4] = { 'P', 'T', 'P', 'V' };
constexpr quint8 kVersion = 1;

void setError(QString* error, const QString& message)
{
    if (nullptr != error)
    {
        *error = message;
    }
}

void appendU32(QByteArray& out, quint32 value)
{
    char buffer[4];
    qToBigEndian(value, buffer);
    out.append(buffer, 4);
}

bool readU32(const QByteArray& in, int offset, quint32& value)
{
    if (offset + 4 > in.size())
    {
        return false;
    }

    value = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(in.constData()) + offset);
    return true;
}

/// AES-256-GCM encrypt. `tag` receives the 16-byte authentication tag.
bool encryptGcm(const QByteArray& key, const QByteArray& iv,
                const QByteArray& plaintext, QByteArray& ciphertext,
                QByteArray& tag)
{
    bool ok = false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();

    if (nullptr == ctx)
    {
        return false;
    }

    // 1. Initialise GCM with a 96-bit IV and the caller's 256-bit key.
    // 2. Encrypt in one update; GCM is a stream mode so the output length
    //    equals the input length.
    // 3. Extract the tag, which authenticates the ciphertext.
    do
    {
        if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr))
        {
            break;
        }
        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr))
        {
            break;
        }
        if (1 != EVP_EncryptInit_ex(
                     ctx, nullptr, nullptr,
                     reinterpret_cast<const unsigned char*>(key.constData()),
                     reinterpret_cast<const unsigned char*>(iv.constData())))
        {
            break;
        }

        ciphertext.resize(plaintext.size());
        int written = 0;

        if (1 != EVP_EncryptUpdate(
                     ctx, reinterpret_cast<unsigned char*>(ciphertext.data()),
                     &written,
                     reinterpret_cast<const unsigned char*>(plaintext.constData()),
                     plaintext.size()))
        {
            break;
        }

        int total = written;
        int finalLen = 0;

        if (1 != EVP_EncryptFinal_ex(
                     ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + total,
                     &finalLen))
        {
            break;
        }

        total += finalLen;
        ciphertext.resize(total);

        tag.resize(SecretStore::kTagBytes);

        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                                     SecretStore::kTagBytes, tag.data()))
        {
            break;
        }

        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

/// AES-256-GCM decrypt. Returns false when the tag does not verify, which is
/// also how a wrong password is detected.
bool decryptGcm(const QByteArray& key, const QByteArray& iv,
                const QByteArray& ciphertext, const QByteArray& tag,
                QByteArray& plaintext)
{
    bool ok = false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();

    if (nullptr == ctx)
    {
        return false;
    }

    do
    {
        if (1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr))
        {
            break;
        }
        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr))
        {
            break;
        }
        if (1 != EVP_DecryptInit_ex(
                     ctx, nullptr, nullptr,
                     reinterpret_cast<const unsigned char*>(key.constData()),
                     reinterpret_cast<const unsigned char*>(iv.constData())))
        {
            break;
        }

        plaintext.resize(ciphertext.size());
        int written = 0;

        if (1 != EVP_DecryptUpdate(
                     ctx, reinterpret_cast<unsigned char*>(plaintext.data()), &written,
                     reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                     ciphertext.size()))
        {
            break;
        }

        // The tag must be set before the final call; that call is what tells us
        // whether the password was right.
        if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(),
                                     const_cast<char*>(tag.constData())))
        {
            break;
        }

        int total = written;
        int finalLen = 0;

        if (1 != EVP_DecryptFinal_ex(
                     ctx, reinterpret_cast<unsigned char*>(plaintext.data()) + total,
                     &finalLen))
        {
            plaintext.clear();
            break;
        }

        total += finalLen;
        plaintext.resize(total);
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

/// PBKDF2-HMAC-SHA256. The password is taken as UTF-8 bytes; the output is a
/// 256-bit key. `keyOut` is overwritten, so callers never append to old key
/// material.
bool deriveKey(const QString& password, const QByteArray& salt,
               int iterations, QByteArray& keyOut)
{
    const QByteArray passwordBytes = password.toUtf8();
    QByteArray key(SecretStore::kKeyBytes, 0);

    const int rc = PKCS5_PBKDF2_HMAC(
        passwordBytes.constData(), passwordBytes.size(),
        reinterpret_cast<const unsigned char*>(salt.constData()), salt.size(),
        iterations, EVP_sha256(), key.size(),
        reinterpret_cast<unsigned char*>(key.data()));

    if (1 != rc)
    {
        return false;
    }

    keyOut = key;
    return true;
}

} // namespace

bool SecretStore::exists(const QString& path)
{
    return QFileInfo::exists(path);
}

bool SecretStore::create(const QString& path, const QJsonObject& payload,
                         const QString& password, QByteArray& keyOut,
                         QString* error)
{
    // 1. Fresh salt and IV for this vault. RAND_bytes fails only when the
    //    system CSPRNG is unavailable, which must abort the operation.
    QByteArray salt(kSaltBytes, 0);
    QByteArray iv(kIvBytes, 0);

    if (1 != RAND_bytes(reinterpret_cast<unsigned char*>(salt.data()), salt.size())
        || 1 != RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), iv.size()))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "system random generator unavailable"));
        return false;
    }

    // 2. Derive the key from the password and encrypt the payload.
    QByteArray key;

    if (!deriveKey(password, salt, kIterations, key))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "key derivation failed"));
        return false;
    }

    QByteArray ciphertext;
    QByteArray tag;

    if (!encryptGcm(key, iv, QJsonDocument(payload).toJson(QJsonDocument::Compact),
                    ciphertext, tag))
    {
        wipe(key);
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "encryption failed"));
        return false;
    }

    // 3. Assemble the header and write atomically with owner-only permissions.
    QByteArray file;
    file.append(kMagic, 4);
    file.append(static_cast<char>(kVersion));
    appendU32(file, static_cast<quint32>(kIterations));
    file.append(salt);
    file.append(iv);
    file.append(tag);
    file.append(ciphertext);

    QSaveFile out(path);

    if (!out.open(QIODevice::WriteOnly))
    {
        wipe(key);
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "cannot open vault: ") + out.errorString());
        return false;
    }

    if (out.write(file) != file.size() || !out.commit())
    {
        wipe(key);
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "cannot write vault: ") + out.errorString());
        return false;
    }

    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);

    keyOut = key;
    return true;
}

std::optional<QJsonObject> SecretStore::unlock(const QString& path,
                                               const QString& password,
                                               QByteArray& keyOut,
                                               QString* error)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "cannot open vault: ") + file.errorString());
        return std::nullopt;
    }

    const QByteArray blob = file.readAll();
    file.close();

    // 1. Validate the header before touching any crypto.
    constexpr int kHeaderMin = 4 + 1 + 4 + kSaltBytes + kIvBytes + kTagBytes;

    if (blob.size() < kHeaderMin || 0 != memcmp(blob.constData(), kMagic, 4))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "vault is corrupt or not a pulseTrader vault"));
        return std::nullopt;
    }

    if (static_cast<quint8>(blob.at(4)) != kVersion)
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "unsupported vault version"));
        return std::nullopt;
    }

    quint32 iterations = 0;

    if (!readU32(blob, 5, iterations) || iterations < 1000)
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "vault header is corrupt"));
        return std::nullopt;
    }

    int offset = 9;
    const QByteArray salt = blob.mid(offset, kSaltBytes);
    offset += kSaltBytes;
    const QByteArray iv = blob.mid(offset, kIvBytes);
    offset += kIvBytes;
    const QByteArray tag = blob.mid(offset, kTagBytes);
    offset += kTagBytes;
    const QByteArray ciphertext = blob.mid(offset);

    // 2. Derive and decrypt. A wrong password shows up here as a tag failure.
    QByteArray key;

    if (!deriveKey(password, salt, static_cast<int>(iterations), key))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "key derivation failed"));
        return std::nullopt;
    }

    QByteArray plaintext;

    if (!decryptGcm(key, iv, ciphertext, tag, plaintext))
    {
        wipe(key);
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "wrong password or corrupted vault"));
        return std::nullopt;
    }

    // Hand the key to the caller; do not wipe the local handle afterwards,
    // because QByteArray shares the buffer (a wipe would clear both).
    keyOut = key;

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(plaintext, &parseError);
    wipe(plaintext);

    if (QJsonParseError::NoError != parseError.error || !document.isObject())
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "vault payload is not valid JSON"));
        return std::nullopt;
    }

    return document.object();
}

bool SecretStore::save(const QString& path, const QJsonObject& payload,
                       const QByteArray& key, QString* error)
{
    // 1. Re-read the header only to keep the salt; the key was derived from it.
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "cannot open vault: ") + file.errorString());
        return false;
    }

    const QByteArray blob = file.readAll();
    file.close();

    if (blob.size() < 9)
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "vault is corrupt"));
        return false;
    }

    const QByteArray salt = blob.mid(9, kSaltBytes);

    // 2. A fresh IV per save: never reuse an (key, IV) pair under GCM.
    QByteArray iv(kIvBytes, 0);

    if (1 != RAND_bytes(reinterpret_cast<unsigned char*>(iv.data()), iv.size()))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "system random generator unavailable"));
        return false;
    }

    QByteArray ciphertext;
    QByteArray tag;

    if (!encryptGcm(key, iv, QJsonDocument(payload).toJson(QJsonDocument::Compact),
                    ciphertext, tag))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "encryption failed"));
        return false;
    }

    QByteArray out;
    out.append(kMagic, 4);
    out.append(static_cast<char>(kVersion));
    appendU32(out, static_cast<quint32>(kIterations));
    out.append(salt);
    out.append(iv);
    out.append(tag);
    out.append(ciphertext);

    QSaveFile saveFile(path);

    if (!saveFile.open(QIODevice::WriteOnly))
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "cannot open vault: ") + saveFile.errorString());
        return false;
    }

    if (saveFile.write(out) != out.size() || !saveFile.commit())
    {
        setError(error, QCoreApplication::translate("pulse::client::SecretStore", "cannot write vault: ") + saveFile.errorString());
        return false;
    }

    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

void SecretStore::wipe(QByteArray& secret)
{
    if (!secret.isEmpty())
    {
        OPENSSL_cleanse(secret.data(), static_cast<size_t>(secret.size()));
        secret.clear();
    }
}

} // namespace pulse::client
