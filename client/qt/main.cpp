#include <QGuiApplication>
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

    // Material adapts best across desktop and touch; the QML layer still owns
    // every colour, so the style only supplies control metrics.
    QQuickStyle::setStyle(QStringLiteral("Material"));

    QQmlApplicationEngine engine;

    // A failed load is fatal: without this the process would sit with an empty
    // window and no explanation.
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("Pulse.Client"), QStringLiteral("Main"));
    return app.exec();
}
