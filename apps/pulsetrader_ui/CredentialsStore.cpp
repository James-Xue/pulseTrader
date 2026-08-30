// credentials_store.cpp — implementation of the .env credential store.
//
// .env format is deliberately kept bash-compatible (KEY=VALUE, one pair per
// line, '#' comments) so run.sh `source .env` keeps working unchanged.

#include "CredentialsStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>
#include <array>

namespace pulse::ui
{

namespace
{

// The canonical (and legacy) environment variable names the engine reads.
constexpr const char *kMainnetKey = "GATE_MAINNET_API_KEY";
constexpr const char *kMainnetSecret = "GATE_MAINNET_API_SECRET";
constexpr const char *kLegacyKey = "GATE_API_KEY";
constexpr const char *kLegacySecret = "GATE_API_SECRET";

// Strip surrounding whitespace, a trailing '\r' (CRLF files), and optional
// matching quotes.
QString cleanValue(const QString &raw)
{
    QString v = raw.trimmed();
    if (v.endsWith('\r'))
    {
        v.chop(1);
        v = v.trimmed();
    }
    if (v.size() >= 2 && (v.startsWith('"') || v.startsWith('\'')))
    {
        const QChar quote = v.at(0);
        if (v.endsWith(quote))
        {
            v = v.mid(1, v.size() - 2);
        }
    }
    return v;
}

// Split a .env file into non-empty, non-comment KEY=VALUE pairs.
QMap<QString, QString> parseEnv(const QString &content)
{
    QMap<QString, QString> vars;
    const QStringList lines = content.split('\n');
    for (const QString &rawLine : lines)
    {
        const QString line = rawLine.trimmed();
        // 1. Skip blank lines and comments
        if (line.isEmpty() || line.startsWith('#'))
        {
            continue;
        }
        // 2. Split on the first '='
        const int eq = line.indexOf('=');
        if (eq <= 0)
        {
            continue;
        }
        const QString name = line.left(eq).trimmed();
        if (name.isEmpty())
        {
            continue;
        }
        vars.insert(name, cleanValue(line.mid(eq + 1)));
    }
    return vars;
}

} // namespace

QString CredentialsStore::resolveEnvPath()
{
    // 1. Prefer the process working directory (typical: launched from the
    //    project root where run.sh expects the file)
    const QString cwdPath = QDir::current().filePath(".env");
    if (QFileInfo::exists(cwdPath))
    {
        return cwdPath;
    }
    // 2. Fall back to the executable's directory
    const QString exePath = QDir(QCoreApplication::applicationDirPath()).filePath(".env");
    if (QFileInfo::exists(exePath))
    {
        return exePath;
    }
    // 3. Neither exists — default to the working directory so a fresh save
    //    lands next to where run.sh will look for it
    return cwdPath;
}

StoredCredentials CredentialsStore::loadFromEnvFile(const QString &path)
{
    StoredCredentials creds;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return creds;
    }
    const QMap<QString, QString> vars = parseEnv(QString::fromUtf8(file.readAll()));

    // Prefer the canonical names, fall back to the legacy ones.
    const QString key = vars.value(QLatin1String(kMainnetKey),
                                   vars.value(QLatin1String(kLegacyKey)));
    const QString secret = vars.value(QLatin1String(kMainnetSecret),
                                      vars.value(QLatin1String(kLegacySecret)));
    creds.apiKey = key;
    creds.apiSecret = secret;
    return creds;
}

QMap<QString, QString> CredentialsStore::loadAllEnvVars(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return {};
    }
    return parseEnv(QString::fromUtf8(file.readAll()));
}

bool CredentialsStore::saveToEnvFile(const QString &path, const QString &key, const QString &secret)
{
    // Names that must end up with the new values (canonical + legacy kept in
    // sync so old launcher scripts keep working).
    const std::array<QString, 4> targets = {
        QLatin1String(kMainnetKey), QLatin1String(kMainnetSecret),
        QLatin1String(kLegacyKey), QLatin1String(kLegacySecret),
    };
    // The value to write per target: key for the two *_KEY names, secret for
    // the two *_SECRET names.
    const std::array<QString, 4> values = { key, secret, key, secret };
    // True once the corresponding name has been seen/appended in the output.
    std::array<bool, 4> written = { false, false, false, false };

    QStringList outLines;

    // 1. Rewrite the existing file line by line, swapping matching names.
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
        for (const QString &rawLine : lines)
        {
            const QString line = rawLine.trimmed();
            const int eq = line.indexOf('=');
            if (eq > 0)
            {
                const QString name = line.left(eq).trimmed();
                const auto it = std::find(targets.begin(), targets.end(), name);
                if (it != targets.end())
                {
                    const std::size_t idx = static_cast<std::size_t>(it - targets.begin());
                    outLines.append(name + QLatin1Char('=') + values.at(idx));
                    written.at(idx) = true;
                    continue;
                }
            }
            outLines.append(rawLine);
        }
        file.close();
    }

    // 2. Append every target name that had no existing line — a legacy-only
    //    file still needs the canonical lines appended (and vice versa).
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        if (false == written.at(i))
        {
            outLines.append(targets.at(i) + QLatin1Char('=') + values.at(i));
        }
    }

    // 3. Write the result (create the file when missing, with a header).
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        return false;
    }
    QTextStream out(&file);
    if (outLines.isEmpty())
    {
        out << "# pulseTrader environment (managed by pulsetrader_ui)\n";
    }
    for (const QString &line : outLines)
    {
        out << line << '\n';
    }
    return true;
}

QString CredentialsStore::maskKey(const QString &key)
{
    if (key.size() <= 8)
    {
        return key;
    }
    return key.left(4) + QString::fromUtf8("…") + key.right(4);
}

} // namespace pulse::ui
