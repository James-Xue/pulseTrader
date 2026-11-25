#pragma once
// external_signal_store.hpp — file inbox/outbox for the 5-minute analysis
// strategy that drives the local Qt paper account.
//
// Files (all under the app data dir, next to sim_account.json):
//   external_signal.json   — written by the external Python signal writer
//   external_ack.json      — last processed signal outcome (written by Qt)
//   external_state.json    — account/position heartbeat (written by Qt)
//   external_signals.jsonl — append-only audit journal (written by Qt)
//
// The app never writes external_signal.json and never lets another process
// write the account files. All reads/writes here run on the UI thread.

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pulse::ui
{

/// Parsed external signal (gold.exec/1). Business validation lives in
/// SimGateSystem; this struct only carries the fields and a parse error.
struct ExternalSignal
{
    QString schema;
    QString signalId;
    QString round;
    QString source;
    QString mode;
    /// Owning strategy; absent/empty legacy signals normalize to "v10".
    QString strategyId = QStringLiteral("v10");
    QDateTime createdAt;
    QDateTime validUntil;
    QString symbol;
    QString action;   ///< none / open / close / modify.
    QString stance;   ///< long / short / flat.
    double referencePrice = 0.0;
    double stop = 0.0;
    QVector<double> targets;
    QString positionId;
    bool closeExisting = false;
    bool triggerConfirmed = false;
    double lots = 0.0;
    double leverage = 0.0;
    double maxDriftUsd = 5.0;
    QString reason;
    QString analysis;
    QString invalidation;
    QString report;
    /// File this signal came from / ack file it expects (per-strategy inbox).
    QString signalPath;
    QString ackPath;
    QJsonObject raw;   ///< Original object (journal/ack context).
    QString error;     ///< Empty when the file parsed and required fields exist.
};

class ExternalSignalStore
{
  public:
    enum class ReadResult
    {
        Missing, ///< No external_signal.json yet.
        Ok,      ///< Parsed; `error` empty.
        Invalid, ///< File exists but is unparsable / missing required fields.
    };

    /// Directory shared with SimAccountStore (PULSETRADER_SIM_DATA_DIR
    /// override, else the account file's directory).
    static QString dataDir();

    static QString signalPath();
    static QString ackPath();
    /// Per-strategy ack path; v10 keeps the legacy external_ack.json.
    static QString ackPathFor(const QString &strategyId);
    static QString statePath();
    static QString journalPath();
    static QString heartbeatPath();

    /// All strategy inboxes: legacy external_signal.json plus
    /// external_signal_<strategy_id>.json files.
    static QStringList pendingSignalPaths();

    /// Read one specific inbox. On Invalid, `signal.error` explains why and
    /// `signal.ackPath` is still populated so the caller can reject it.
    static ReadResult readSignalAt(const QString &path, ExternalSignal &signal);

    /// Read the legacy external_signal.json inbox.
    static ReadResult readSignal(ExternalSignal &signal);

    /// Atomic write (QSaveFile) of the ack / state JSON.
    static bool writeAck(const QJsonObject &ack);
    static bool writeAckAt(const QString &path, const QJsonObject &ack);
    static bool writeState(const QJsonObject &state);

    /// Append one compact JSON line to external_signals.jsonl.
    static bool appendJournal(const QJsonObject &event);

    /// Load recent_signal_ids from external_state.json (empty when absent).
    static QStringList loadRecentSignalIds();

    /// Read xau_5m_heartbeat.json (written by xau_signal.py every round).
    /// Missing/invalid file returns an empty object and sets *error.
    static QJsonObject readHeartbeat(QString *error = nullptr);

  private:
    static QJsonObject readJsonFile(const QString &path, QString *error);
    static bool writeJsonAtomic(const QString &path, const QJsonObject &object);
};

} // namespace pulse::ui
