#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QProcess;

namespace pulse::client
{

/// Owns the engine process started by the Qt client.
///
/// Lifecycle:
/// 1. start() writes a per-run TOML config (0600) into the user runtime
///    directory, spawns `pulsetrader --config <path>` and leaves the engine
///    running until stop() or process exit.
/// 2. The config carries the exchange credentials from the unlocked vault; it
///    lives on tmpfs (/run/user/<uid>) and is deleted on stop.
/// 3. The engine runs with signal_only=true, so strategies emit signals but
///    never place orders. The client is a monitor.
///
/// Thread affinity: GUI thread only. QProcess delivers readyRead/finished on
/// the thread that created it.
class EngineLauncher : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(int port READ port NOTIFY runningChanged)

public:
    explicit EngineLauncher(QObject* parent = nullptr);
    ~EngineLauncher() override;

    /// Write the config and spawn the engine. Returns false with lastError()
    /// set when the engine binary cannot be found or the process fails to
    /// start. `proxy` may be empty.
    Q_INVOKABLE bool start(const QString& apiKey, const QString& apiSecret,
                           const QString& network, const QString& market,
                           const QString& proxy, const QString& symbols,
                           int port);

    /// Terminate the engine (SIGTERM, then SIGKILL after five seconds) and
    /// delete the runtime config that contains the credentials.
    Q_INVOKABLE void stop();

    [[nodiscard]] bool running() const { return m_running; }
    [[nodiscard]] QString lastError() const { return m_lastError; }
    [[nodiscard]] int port() const { return m_port; }

    /// Build the engine TOML for a profile. Exposed as a static function so
    /// the unit test can assert on it without spawning a process.
    [[nodiscard]] static QString buildConfig(const QString& apiKey,
                                             const QString& apiSecret,
                                             const QString& network,
                                             const QString& market,
                                             const QString& proxy,
                                             const QString& symbols,
                                             int port,
                                             const QString& runtimeDir);

    /// Locate the engine binary: PULSE_ENGINE_BIN, then the build trees next
    /// to the client, then PATH. Empty when nothing executable was found.
    [[nodiscard]] static QString findEngineBinary();

signals:
    void runningChanged();
    void lastErrorChanged();

private:
    void setError(const QString& message);
    void onProcessFinished(int exitCode);
    void cleanupRuntimeFiles();

    QProcess* m_process = nullptr;
    QString m_configPath;
    bool m_running = false;
    int m_port = 8081;
    QString m_lastError;
};

} // namespace pulse::client
