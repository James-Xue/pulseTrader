// external_signal_store.cpp — JSON inbox/outbox for the external strategy.

#include "ExternalSignalStore.hpp"

#include "SimAccountStore.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>

namespace pulse::ui
{

namespace
{

QString resolveDataDir()
{
    const QString overrideDir = qEnvironmentVariable("PULSETRADER_SIM_DATA_DIR");
    if (false == overrideDir.isEmpty())
    {
        QDir().mkpath(overrideDir);
        return QDir(overrideDir).absolutePath();
    }
    const QString accountDir = QFileInfo(SimAccountStore::filePath()).absolutePath();
    QDir().mkpath(accountDir);
    return accountDir;
}

QString filePath(const QString &name)
{
    return QDir(resolveDataDir()).filePath(name);
}

QDateTime parseDateTime(const QString &text)
{
    QDateTime value = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (false == value.isValid())
    {
        value = QDateTime::fromString(text, Qt::ISODate);
    }
    if (false == value.isValid())
    {
        value = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    return value;
}

bool validAction(const QString &action)
{
    return action == QStringLiteral("none") || action == QStringLiteral("open")
        || action == QStringLiteral("close") || action == QStringLiteral("modify");
}

QString sanitizedStrategyId(QString strategyId)
{
    strategyId = strategyId.trimmed();
    if (strategyId.isEmpty())
    {
        return QStringLiteral("v10");
    }
    QString out;
    for (const QChar ch : strategyId)
    {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('_')
            || ch == QLatin1Char('-') || ch == QLatin1Char('.'))
        {
            out.append(ch);
        }
        else
        {
            out.append(QLatin1Char('_'));
        }
    }
    if (out.isEmpty())
    {
        out = QStringLiteral("v10");
    }
    return out.left(64);
}

QString strategyFromSignalPath(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    if (name == QStringLiteral("external_signal.json"))
    {
        return QStringLiteral("v10");
    }
    const QString prefix = QStringLiteral("external_signal_");
    if (name.startsWith(prefix) && name.endsWith(QStringLiteral(".json")))
    {
        return sanitizedStrategyId(
            name.mid(prefix.size(), name.size() - prefix.size() - 5));
    }
    return QStringLiteral("v10");
}

} // namespace

QString ExternalSignalStore::dataDir()
{
    return resolveDataDir();
}

QString ExternalSignalStore::signalPath()
{
    return filePath(QStringLiteral("external_signal.json"));
}

QString ExternalSignalStore::ackPath()
{
    return filePath(QStringLiteral("external_ack.json"));
}

QString ExternalSignalStore::ackPathFor(const QString &strategyId)
{
    const QString owner = sanitizedStrategyId(strategyId);
    if (owner == QStringLiteral("v10"))
    {
        return ackPath();
    }
    return filePath(QStringLiteral("external_ack_%1.json").arg(owner));
}

QStringList ExternalSignalStore::pendingSignalPaths()
{
    QStringList result;
    QDir dir(resolveDataDir());
    if (dir.exists(QStringLiteral("external_signal.json")))
    {
        result.append(dir.filePath(QStringLiteral("external_signal.json")));
    }
    const QStringList names = dir.entryList(
        QStringList{QStringLiteral("external_signal_*.json")},
        QDir::Files, QDir::Name);
    for (const QString &name : names)
    {
        if (name.endsWith(QStringLiteral(".tmp"))
            || name.contains(QStringLiteral(".json.tmp")))
        {
            continue;
        }
        result.append(dir.filePath(name));
    }
    return result;
}

QString ExternalSignalStore::statePath()
{
    return filePath(QStringLiteral("external_state.json"));
}

QString ExternalSignalStore::journalPath()
{
    return filePath(QStringLiteral("external_signals.jsonl"));
}

QString ExternalSignalStore::heartbeatPath()
{
    return filePath(QStringLiteral("xau_5m_heartbeat.json"));
}

ExternalSignalStore::ReadResult ExternalSignalStore::readSignalAt(
    const QString &path, ExternalSignal &signal)
{
    signal.signalPath = path;
    signal.strategyId = strategyFromSignalPath(path);
    signal.ackPath = ackPathFor(signal.strategyId);
    QFile file(path);
    if (false == file.exists())
    {
        return ReadResult::Missing;
    }
    if (false == file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        signal.error = QStringLiteral("无法读取信号文件：%1").arg(path);
        return ReadResult::Invalid;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (QJsonParseError::NoError != parseError.error || false == doc.isObject())
    {
        signal.error = QStringLiteral("信号 JSON 解析失败：%1").arg(parseError.errorString());
        return ReadResult::Invalid;
    }

    const QJsonObject obj = doc.object();
    signal.raw = obj;
    signal.schema = obj.value(QStringLiteral("schema")).toString();
    signal.signalId = obj.value(QStringLiteral("signal_id")).toString();
    signal.round = obj.value(QStringLiteral("round")).toString();
    signal.source = obj.value(QStringLiteral("source")).toString();
    signal.mode = obj.value(QStringLiteral("mode")).toString();
    signal.strategyId = obj.value(QStringLiteral("strategy_id"))
                            .toString(signal.strategyId);
    if (signal.strategyId.isEmpty())
    {
        signal.strategyId = strategyFromSignalPath(path);
    }
    signal.strategyId = sanitizedStrategyId(signal.strategyId);
    signal.ackPath = ackPathFor(signal.strategyId);
    signal.createdAt =
        parseDateTime(obj.value(QStringLiteral("created_at")).toString());
    signal.validUntil =
        parseDateTime(obj.value(QStringLiteral("valid_until")).toString());
    signal.symbol = obj.value(QStringLiteral("symbol")).toString();
    signal.action = obj.value(QStringLiteral("action")).toString();
    signal.stance = obj.value(QStringLiteral("stance")).toString();
    signal.referencePrice = obj.value(QStringLiteral("reference_price")).toDouble(0.0);
    signal.stop = obj.value(QStringLiteral("stop")).toDouble(0.0);
    signal.positionId = obj.value(QStringLiteral("position_id")).toString();
    signal.closeExisting = obj.value(QStringLiteral("close_existing")).toBool(false);
    signal.reason = obj.value(QStringLiteral("reason")).toString();
    signal.analysis = obj.value(QStringLiteral("analysis")).toString();
    signal.invalidation = obj.value(QStringLiteral("invalidation")).toString();
    signal.report = obj.value(QStringLiteral("report")).toString();

    const QJsonArray targets = obj.value(QStringLiteral("targets")).toArray();
    for (const QJsonValue &value : targets)
    {
        if (value.isDouble())
        {
            signal.targets.push_back(value.toDouble());
        }
    }

    const QJsonObject sizing = obj.value(QStringLiteral("sizing")).toObject();
    signal.lots = sizing.value(QStringLiteral("lots")).toDouble(0.0);
    signal.leverage = obj.value(QStringLiteral("leverage")).toDouble(0.0);
    if (signal.leverage <= 0.0)
    {
        signal.leverage = sizing.value(QStringLiteral("leverage")).toDouble(0.0);
    }

    const QJsonObject trigger = obj.value(QStringLiteral("trigger")).toObject();
    signal.triggerConfirmed = trigger.value(QStringLiteral("confirmed")).toBool(false);
    signal.maxDriftUsd = trigger.value(QStringLiteral("max_drift_usd")).toDouble(0.0);
    if (signal.maxDriftUsd <= 0.0)
    {
        signal.maxDriftUsd = obj.value(QStringLiteral("max_drift_usd")).toDouble(5.0);
    }
    if (signal.maxDriftUsd <= 0.0)
    {
        signal.maxDriftUsd = 5.0;
    }

    if (signal.schema != QStringLiteral("gold.exec/1"))
    {
        signal.error = QStringLiteral("schema 不是 gold.exec/1：%1").arg(signal.schema);
        return ReadResult::Invalid;
    }
    if (signal.signalId.isEmpty())
    {
        signal.error = QStringLiteral("signal_id 为空");
        return ReadResult::Invalid;
    }
    if (false == validAction(signal.action))
    {
        signal.error = QStringLiteral("未知 action：%1").arg(signal.action);
        return ReadResult::Invalid;
    }
    if (signal.source.isEmpty() || signal.mode.isEmpty() || signal.symbol.isEmpty())
    {
        signal.error = QStringLiteral("source/mode/symbol 缺失");
        return ReadResult::Invalid;
    }
    if (false == signal.createdAt.isValid() || false == signal.validUntil.isValid())
    {
        signal.error = QStringLiteral("created_at / valid_until 不是有效时间");
        return ReadResult::Invalid;
    }
    if (signal.action == QStringLiteral("open"))
    {
        if (signal.referencePrice <= 0.0 || signal.stop <= 0.0 || signal.targets.isEmpty())
        {
            signal.error = QStringLiteral("open 缺少 reference_price/stop/targets");
            return ReadResult::Invalid;
        }
    }
    else if (signal.action == QStringLiteral("modify"))
    {
        if (signal.stop <= 0.0 || signal.targets.isEmpty())
        {
            signal.error = QStringLiteral("modify 缺少 stop/targets");
            return ReadResult::Invalid;
        }
    }

    return ReadResult::Ok;
}

ExternalSignalStore::ReadResult ExternalSignalStore::readSignal(ExternalSignal &signal)
{
    return readSignalAt(signalPath(), signal);
}

bool ExternalSignalStore::writeAck(const QJsonObject &ack)
{
    return writeJsonAtomic(ackPath(), ack);
}

bool ExternalSignalStore::writeAckAt(const QString &path, const QJsonObject &ack)
{
    return writeJsonAtomic(path, ack);
}

bool ExternalSignalStore::writeState(const QJsonObject &state)
{
    return writeJsonAtomic(statePath(), state);
}

bool ExternalSignalStore::appendJournal(const QJsonObject &event)
{
    QDir().mkpath(dataDir());
    QFile file(journalPath());
    if (false == file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return false;
    }
    file.write(QJsonDocument(event).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.close();
    return true;
}

QStringList ExternalSignalStore::loadRecentSignalIds()
{
    QString error;
    const QJsonObject state = readJsonFile(statePath(), &error);
    QStringList ids;
    if (false == error.isEmpty())
    {
        return ids;
    }
    const QJsonArray recent = state.value(QStringLiteral("recent_signal_ids")).toArray();
    for (const QJsonValue &value : recent)
    {
        if (value.isString())
        {
            ids.push_back(value.toString());
        }
    }
    return ids;
}

QJsonObject ExternalSignalStore::readHeartbeat(QString *error)
{
    return readJsonFile(heartbeatPath(), error);
}

QJsonObject ExternalSignalStore::readJsonFile(const QString &path, QString *error)
{
    QFile file(path);
    if (false == file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        if (nullptr != error)
        {
            *error = QStringLiteral("无法读取：%1").arg(path);
        }
        return {};
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (QJsonParseError::NoError != parseError.error || false == doc.isObject())
    {
        if (nullptr != error)
        {
            *error = QStringLiteral("JSON 解析失败：%1").arg(parseError.errorString());
        }
        return {};
    }
    if (nullptr != error)
    {
        error->clear();
    }
    return doc.object();
}

bool ExternalSignalStore::writeJsonAtomic(const QString &path, const QJsonObject &object)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (false == file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

} // namespace pulse::ui
