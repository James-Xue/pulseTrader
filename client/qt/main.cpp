#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QWindow>

#include "i18n/Translator.hpp"

/// Application entry point.
///
/// The QML module `Pulse.Client` is compiled into the executable by
/// qt_add_qml_module, so no import path or resource registration is needed
/// here. EngineController is a QML_ELEMENT in that module and is instantiated
/// from Main.qml, which keeps the C++ side free of view wiring.
///
/// Besides the normal interactive launch it supports a headless capture mode
/// (`--screenshot <path>`), which renders the loaded window to a PNG and exits.
/// That is what produces the images used in the README; it also gives CI a way
/// to prove the UI still renders.
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

    // 3. Command line.
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("pulseTrader read-only control console"));
    parser.addHelpOption();

    const QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"),
        QStringLiteral("Render the window to <path> (PNG) and exit."),
        QStringLiteral("path"));
    const QCommandLineOption delayOption(
        QStringLiteral("screenshot-delay"),
        QStringLiteral("Milliseconds to wait before capturing (default 1500)."),
        QStringLiteral("ms"), QStringLiteral("1500"));
    const QCommandLineOption darkOption(
        QStringLiteral("dark"),
        QStringLiteral("Force the dark colour scheme for this run."));
    const QCommandLineOption lightOption(
        QStringLiteral("light"),
        QStringLiteral("Force the light colour scheme for this run."));
    const QCommandLineOption pageOption(
        QStringLiteral("page"),
        QStringLiteral("Open the shell on the 0-based sidebar page <index>."),
        QStringLiteral("index"));
    const QCommandLineOption selectOption(
        QStringLiteral("select-strategy"),
        QStringLiteral("Select a strategy instance by id after loading."),
        QStringLiteral("id"));
    const QCommandLineOption sizeOption(
        QStringLiteral("window-size"),
        QStringLiteral("Resize the window to <width>x<height> for this run."),
        QStringLiteral("WxH"));
    parser.addOption(screenshotOption);
    parser.addOption(delayOption);
    parser.addOption(darkOption);
    parser.addOption(lightOption);
    parser.addOption(pageOption);
    parser.addOption(selectOption);
    parser.addOption(sizeOption);
    parser.process(app);

    // 4. Basic is the neutral base style: it draws almost nothing, so the QML
    //    layer owns the entire macOS-like look. Material/Fusion would fight
    //    the custom palette on every control.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // One application font so controls and text share the same metrics. Noto
    // Sans is the closest neutral grotesque installed here; Qt falls back to
    // Noto Sans CJK for Chinese glyphs.
    QFont font(QStringLiteral("Noto Sans"), 13);
    QGuiApplication::setFont(font);

    // 5. Interface language. The singleton must exist (and have installed its
    //    catalogue) before the first QML binding runs: otherwise the first
    //    frame would render English strings that only retranslate on click.
    //    The QML singleton factory later returns this same instance.
    pulse::client::Translator::instance();

    QQmlApplicationEngine engine;

    // A failed load is fatal: without this the process would sit with an empty
    // window and no explanation.
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("Pulse.Client"), QStringLiteral("Main"));

    // 6. Theme override for captures. Main.qml restores the saved preference in
    //    Component.onCompleted, which has already run by now, so an explicit
    //    flag wins without being overwritten.
    if (parser.isSet(darkOption) || parser.isSet(lightOption))
    {
        if (auto* style = engine.singletonInstance<QObject*>(
                QStringLiteral("Pulse.Client"), QStringLiteral("Style")))
        {
            style->setProperty("dark", parser.isSet(darkOption));
        }
    }

    // 6b. Optional start page. Index 5 is Gold Traders in the current sidebar
    //     order. The shell is built at load time even while hidden, so the
    //     stack exists for a capture that runs before any user interaction.
    if (parser.isSet(pageOption))
    {
        bool ok = false;
        const int index = parser.value(pageOption).toInt(&ok);
        if (ok && index >= 0)
        {
            for (QObject* rootObject : engine.rootObjects())
            {
                if (auto* stack = rootObject->findChild<QObject*>(
                        QStringLiteral("pagesStack")))
                {
                    stack->setProperty("currentIndex", index);
                    break;
                }
            }
        }
    }

    // 6c. Optional preselection for captures. The controller defers the
    //     parameter fetch until the control socket is ready, so this is safe
    //     before the first poll completes.
    if (parser.isSet(selectOption))
    {
        const QString strategyId = parser.value(selectOption);
        for (QObject* rootObject : engine.rootObjects())
        {
            if (auto* controller = rootObject->findChild<QObject*>(
                    QStringLiteral("engineController")))
            {
                QMetaObject::invokeMethod(controller, "selectStrategy",
                                          Q_ARG(QString, strategyId));
                break;
            }
        }
    }

    // 6d. Optional capture size. The default window is 1100x720, which shows only
    //     the top of a long page; a taller window lets a single grab include
    //     every card.
    if (parser.isSet(sizeOption))
    {
        const QStringList parts = parser.value(sizeOption).split(QLatin1Char('x'));
        if (2 == parts.size())
        {
            const int width = parts.at(0).toInt();
            const int height = parts.at(1).toInt();
            if (width > 0 && height > 0)
            {
                for (QObject* rootObject : engine.rootObjects())
                {
                    if (auto* window = qobject_cast<QQuickWindow*>(rootObject))
                    {
                        window->resize(width, height);
                        break;
                    }
                }
            }
        }
    }

    // 7. Headless capture. The delay lets the first frame, the poll responses
    //    and any layout settle before the grab.
    if (parser.isSet(screenshotOption))
    {
        const QString path = parser.value(screenshotOption);
        const int delayMs = parser.value(delayOption).toInt();

        QTimer::singleShot(delayMs, &app, [&engine, path]()
        {
            if (engine.rootObjects().isEmpty())
            {
                QCoreApplication::exit(2);
                return;
            }
            auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (nullptr == window)
            {
                QCoreApplication::exit(3);
                return;
            }
            const QImage frame = window->grabWindow();
            QCoreApplication::exit(frame.isNull() || !frame.save(path) ? 4 : 0);
        });
    }

    return app.exec();
}
