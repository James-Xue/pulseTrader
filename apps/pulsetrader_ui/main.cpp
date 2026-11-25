// main.cpp — pulsetrader_ui launcher entry point
//
// Startup flow:
//   1. Show the login dialog (API key/secret + remember-me)
//   2. On a successful verification, show the "模拟 Gate 系统" control
//      window, which manages the trading engine's lifecycle
//
// The launcher is a separate process on purpose: the engine itself stays
// headless and is spawned with the verified credentials injected into its
// environment, so no engine code is touched.

#include <QApplication>
#include <QTimer>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QCoreApplication> // Qt5: opt-in high-DPI support
#endif

#include "LoginDialog.hpp"
#include "MainWindow.hpp"

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("pulseTrader"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("pulseTrader"));

    const QStringList args = app.arguments();
    const bool autoLogin = args.contains(QStringLiteral("--auto-login"));
    const bool autoStartEngine = args.contains(QStringLiteral("--auto-start-engine"));

    pulse::ui::LoginDialog login;
    if (autoLogin)
    {
        // Uses the credentials remembered in .env; if they are missing or
        // invalid the dialog stays visible for manual correction.
        login.setAutoLogin(true);
        QTimer::singleShot(0, &login, &pulse::ui::LoginDialog::autoLogin);
    }
    if (QDialog::Accepted != login.exec())
    {
        return 0;
    }

    // The main window is a pure Qt Designer form (MainWindow.ui); the login
    // credentials stay in .env (saved by the dialog when "remember" was
    // checked) and the engine start button will read them from there.
    SimGateSystem window(login.verifyResult(), login.apiKey(), login.apiSecret());
    window.show();
    if (autoStartEngine)
    {
        // Start after the window is visible. If xau-5m is not running yet the
        // engine keeps polling prices and picks up signals once its heartbeat
        // appears.
        QTimer::singleShot(0, &window, &SimGateSystem::startEngine);
    }
    return app.exec();
}
