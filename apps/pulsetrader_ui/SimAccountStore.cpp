// sim_account_store.cpp — implementation of the simulated-account store.

#include "SimAccountStore.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace pulse::ui
{

namespace
{

// The JSON document root key holding per-key accounts.
constexpr const char *kAccountsKey = "accounts";
constexpr const char *kBalanceKey = "balance_usdt";
constexpr const char *kUpdatedAtKey = "updated_at";
constexpr const char *kPositionsKey = "positions";

// Serialise one open position into a JSON object.
QJsonObject positionToJson(const SimPosition &p)
{
    QJsonObject obj;
    obj[QLatin1String("id")] = p.id;
    obj[QLatin1String("side")] = p.side;
    obj[QLatin1String("volume")] = p.volumeLots;
    obj[QLatin1String("entry")] = p.entryPrice;
    obj[QLatin1String("tp")] = p.takeProfit;
    obj[QLatin1String("sl")] = p.stopLoss;
    obj[QLatin1String("margin")] = p.marginUsdt;
    obj[QLatin1String("fee")] = p.feeUsdt;
    obj[QLatin1String("leverage")] = p.leverage;
    obj[QLatin1String("opened_at_ms")] = static_cast<double>(p.openedAtMs);
    return obj;
}

// Parse one position object back (lenient: missing fields → defaults).
SimPosition positionFromJson(const QJsonObject &obj)
{
    SimPosition p;
    p.id = obj.value(QLatin1String("id")).toString();
    p.side = obj.value(QLatin1String("side")).toString(QStringLiteral("long"));
    p.volumeLots = obj.value(QLatin1String("volume")).toDouble(0.0);
    p.entryPrice = obj.value(QLatin1String("entry")).toDouble(0.0);
    p.takeProfit = obj.value(QLatin1String("tp")).toDouble(0.0);
    p.stopLoss = obj.value(QLatin1String("sl")).toDouble(0.0);
    p.marginUsdt = obj.value(QLatin1String("margin")).toDouble(0.0);
    p.feeUsdt = obj.value(QLatin1String("fee")).toDouble(0.0);
    p.leverage = obj.value(QLatin1String("leverage")).toDouble(1.0);
    p.openedAtMs = static_cast<std::int64_t>(
        obj.value(QLatin1String("opened_at_ms")).toDouble(0.0));
    return p;
}

QString resolveFilePath()
{
    // 1. data/ under the process working directory (project root when
    //    launched from the repo — matches the engine's data/ convention).
    const QString cwdPath = QDir::current().filePath(QStringLiteral("data/sim_account.json"));
    if (QDir(cwdPath).exists() || QDir(QDir::current().filePath(QStringLiteral("data"))).exists())
    {
        return cwdPath;
    }
    // 2. data/ under the executable's directory (double-clicked launcher).
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("data/sim_account.json"));
}

} // namespace

QString SimAccountStore::filePath()
{
    return resolveFilePath();
}

std::optional<SimAccount> SimAccountStore::load(const QString &apiKey)
{
    if (apiKey.isEmpty())
    {
        return std::nullopt;
    }
    QFile file(resolveFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return std::nullopt;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (QJsonParseError::NoError != parseError.error || false == doc.isObject())
    {
        return std::nullopt;
    }
    const QJsonObject root = doc.object();
    if (false == root.contains(QLatin1String(kAccountsKey))
        || false == root[QLatin1String(kAccountsKey)].isObject())
    {
        return std::nullopt;
    }
    const QJsonObject accounts = root[QLatin1String(kAccountsKey)].toObject();
    if (false == accounts.contains(apiKey) || false == accounts[apiKey].isObject())
    {
        return std::nullopt;
    }
    const QJsonObject entry = accounts[apiKey].toObject();
    SimAccount account;
    account.balanceUsdt = entry.value(QLatin1String(kBalanceKey)).toDouble(0.0);
    account.updatedAt = entry.value(QLatin1String(kUpdatedAtKey)).toString();
    if (entry.contains(QLatin1String(kPositionsKey)) && entry[QLatin1String(kPositionsKey)].isArray())
    {
        const QJsonArray arr = entry[QLatin1String(kPositionsKey)].toArray();
        account.positions.reserve(static_cast<std::size_t>(arr.size()));
        for (const auto &value : arr)
        {
            if (value.isObject())
            {
                account.positions.push_back(positionFromJson(value.toObject()));
            }
        }
    }
    return account;
}

SimAccountStore::DebitResult SimAccountStore::debit(const QString &apiKey, double amountUsdt)
{
    DebitResult result;
    if (amountUsdt <= 0.0)
    {
        result.error = QStringLiteral("扣减金额无效（%1 USDT）").arg(amountUsdt);
        return result;
    }
    const auto account = load(apiKey);
    if (false == account.has_value())
    {
        result.error = QStringLiteral("未设置模拟账户，请先点击“模拟账户”设置初始余额");
        return result;
    }
    const double remaining = account->balanceUsdt - amountUsdt;
    if (remaining < 0.0)
    {
        result.error = QStringLiteral("模拟账户余额不足（需 %1 USDT，剩余 %2 USDT）")
                           .arg(amountUsdt, 0, 'f', 2)
                           .arg(account->balanceUsdt, 0, 'f', 2);
        return result;
    }
    if (false == save(apiKey, remaining))
    {
        result.error = QStringLiteral("模拟账户余额保存失败：%1").arg(filePath());
        return result;
    }
    result.ok = true;
    result.remainingUsdt = remaining;
    return result;
}

bool SimAccountStore::save(const QString &apiKey, double balanceUsdt)
{
    // Preserve open positions across balance updates (paper trades).
    SimAccount account;
    const auto existing = load(apiKey);
    if (existing.has_value())
    {
        account = *existing;
    }
    account.balanceUsdt = balanceUsdt;
    account.updatedAt =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    return saveAccount(apiKey, account);
}

bool SimAccountStore::saveAccount(const QString &apiKey, const SimAccount &account)
{
    if (apiKey.isEmpty())
    {
        return false;
    }
    const QString path = resolveFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    // 1. Read the existing document so other keys' accounts survive.
    QJsonObject root;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (QJsonParseError::NoError == parseError.error && doc.isObject())
        {
            root = doc.object();
        }
        file.close();
    }

    // 2. Upsert this key's entry (balance + positions).
    QJsonObject accounts = root[QLatin1String(kAccountsKey)].toObject();
    QJsonObject entry;
    entry[QLatin1String(kBalanceKey)] = account.balanceUsdt;
    entry[QLatin1String(kUpdatedAtKey)] = account.updatedAt;
    QJsonArray positions;
    for (const auto &p : account.positions)
    {
        positions.append(positionToJson(p));
    }
    entry[QLatin1String(kPositionsKey)] = positions;
    accounts[apiKey] = entry;
    root[QLatin1String(kAccountsKey)] = accounts;

    // 3. Write back (atomic-ish: tmp + rename keeps a crash from
    //    truncating the store).
    const QString tmpPath = path + QStringLiteral(".tmp");
    QFile out(tmpPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        return false;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();
    // Remove the target first: QFile::remove() reports false for a missing
    // file (first save), and QFile::rename() cannot overwrite an existing
    // file on Windows. A missing target is fine here — rename still moves
    // the tmp file into place.
    QFile::remove(path);
    return QFile::rename(tmpPath, path);
}

} // namespace pulse::ui
