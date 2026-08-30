#pragma once
// credentials_store.hpp — persistence for Gate.io API credentials (.env file)
//
// The engine already reads its credentials from environment variables
// (GATE_MAINNET_API_KEY / GATE_MAINNET_API_SECRET, sourced by run.sh from a
// root-level .env file). This module reuses that same file as the "remember
// me" store:
//
//   1. LoginDialog pre-fills the form from .env when it exists
//   2. A successful login writes the new key/secret back into .env,
//      preserving every other line (PULSE_NETWORK, AI_API_KEY, ...)
//   3. MainWindow re-reads .env (via loadAllEnvVars) so the engine child
//      process inherits the exact environment run.sh would have sourced
//
// Thread safety: all functions are static and touch only their own file
// handles — safe to call from any thread.

#include <QMap>
#include <QString>

namespace pulse::ui
{

struct StoredCredentials
{
    QString apiKey;
    QString apiSecret;
};

class CredentialsStore
{
  public:
    /// Locate the .env file: process working directory first, then the
    /// executable's directory. Returns an empty string if neither exists.
    static QString resolveEnvPath();

    /// Parse KEY=VALUE lines from a .env file.
    ///
    /// Blank lines and lines starting with '#' are skipped; the value is
    /// stripped of surrounding whitespace and optional quotes. Prefers
    /// GATE_MAINNET_API_KEY/SECRET, falls back to the legacy GATE_API_KEY/
    /// GATE_API_SECRET names.
    static StoredCredentials loadFromEnvFile(const QString &path);

    /// Read every KEY=VALUE pair from a .env file (used to seed the engine
    /// child process environment). Returns an empty map when the file does
    /// not exist or cannot be read.
    static QMap<QString, QString> loadAllEnvVars(const QString &path);

    /// Write key/secret into a .env file, preserving all other lines.
    ///
    /// Replaces both GATE_MAINNET_* and the legacy GATE_* names in place;
    /// missing lines are appended. Creates the file (with a header comment)
    /// when it does not exist yet. Returns false on write failure.
    static bool saveToEnvFile(const QString &path, const QString &key, const QString &secret);

    /// Human-readable account identity: first 4 + last 4 characters of the
    /// key (e.g. "ab12…cd34"). The full key is never shown. Keys shorter
    /// than 9 characters are returned verbatim.
    static QString maskKey(const QString &key);
};

} // namespace pulse::ui
