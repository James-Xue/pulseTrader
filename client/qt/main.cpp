#include <QGuiApplication>
#include <QFont>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

/// Application entry point.
///
/// The QML module `Pulse.Client` is compiled into the executable by
/// qt_add_qml_module, so no import path or resource registration is needed
/// here. EngineController is a QML_ELEMENT in that module and is instantiated
/// from Main.qml, which keeps the C++ side free of view wiring.
int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    // 1. Identity. The desktop file name is what links the Wayland window to
    //    its .desktop entry; GNOME resolves the dock icon from that file, not
    //    from setWindowIcon(). It must match packaging/linux/pulsetrader.desktop.
    QGuiApplication::setApplicationName(QStringLiteral("pulseTrader"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("pulseTrader"));
    QGuiApplication::setOrganizationName(QStringLiteral("pulseTrader"));
    QGuiApplication::setDesktopFileName(QStringLiteral("pulsetrader"));

    // 2. Window icon for X11 and for the compositor's fallback path. The path
    //    is the QML module resource prefix plus the RESOURCES entry, not the
    //    bare "qrc:/resources/..." form.
    QGuiApplication::setWindowIcon(
        QIcon(QStringLiteral(":/qt/qml/Pulse/Client/resources/icons/app.svg")));

    // 3. Basic is the neutral base style: it draws almost nothing, so the QML
    //    layer owns the entire macOS-like look. Material/Fusion would fight
    //    the custom palette on every control.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // One application font so controls and text share the same metrics. Noto
    // Sans is the closest neutral grotesque installed here; Qt falls back to
    // Noto Sans CJK for Chinese glyphs.
    QFont font(QStringLiteral("Noto Sans"), 13);
    QGuiApplication::setFont(font);

    QQmlApplicationEngine engine;

    // A failed load is fatal: without this the process would sit with an empty
    // window and no explanation.
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("Pulse.Client"), QStringLiteral("Main"));
    return app.exec();
}
