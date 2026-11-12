#pragma once

#include <QJSEngine>
#include <QObject>
#include <QString>
#include <QTranslator>
#include <QtQml/qqmlregistration.h>
#include <QQmlEngine>

namespace pulse::client
{

/// Runtime interface-language switch for the whole client.
///
/// One process-wide instance owns a QTranslator and swaps it in and out of the
/// application. Every user-visible string in the QML layer already goes through
/// qsTr(), so installing a translator and asking the QML engine to retranslate
/// re-evaluates every binding without restarting the client.
///
/// Thread affinity: GUI thread only. QTranslator install/remove and
/// QQmlEngine::retranslate() are not safe to call from worker threads.
///
/// Lifetime: created in main() before the QML engine loads (so the first frame
/// is already translated); the QML singleton factory returns that same
/// instance, and CppOwnership keeps the engine from deleting it.
class Translator : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)

public:
    /// Deliberately NOT default-constructible. Qt's QML singleton registration
    /// prefers a default constructor over the create() factory
    /// (qqmlprivate.h: singletonConstructionMode()), which would build a second
    /// instance that has no QQmlEngine and therefore can never retranslate the
    /// UI. Keeping the parent argument mandatory forces the factory path.
    explicit Translator(QObject* parent);
    ~Translator() override;

    /// QML singleton factory. Returns the instance created in main() so the
    /// translator is installed before the first qsTr() binding is evaluated.
    static Translator* create(QQmlEngine* engine, QJSEngine* jsEngine);

    /// Process-wide instance. The first call reads the saved preference (or the
    /// system locale when nothing is saved) and installs the translator.
    static Translator* instance();

    /// Current language: "en" (source language) or "zh" (Simplified Chinese).
    [[nodiscard]] QString language() const;

    /// Switch the interface language. Unknown values fall back to English.
    /// Persists the choice, then retranslates every live QML binding.
    void setLanguage(const QString& language);

signals:
    /// Emitted after the translator has been swapped and the language stored.
    void languageChanged();

private:
    /// Map any external spelling ("zh", "zh_CN", "zh-Hans", ...) onto the two
    /// languages the client ships.
    static QString canonical(const QString& language);

    /// Language to start in: the saved choice, else the system locale.
    static QString initialLanguage();

    /// Install the translator for \p language (or remove it for English) and
    /// re-evaluate QML string bindings.
    void applyLanguage(const QString& language);

    QTranslator m_translator;
    QQmlEngine* m_engine = nullptr;
    QString m_language;
};

} // namespace pulse::client