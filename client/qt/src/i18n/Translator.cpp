#include "i18n/Translator.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QLocale>
#include <QSettings>

namespace pulse::client
{

namespace
{

/// Compiled catalogue embedded by CMake under the :/i18n resource prefix.
constexpr auto kChineseCatalog = ":/i18n/pulsetrader_zh_CN.qm";

/// QSettings location of the saved choice. The group matches the QML
/// `Settings { category: "appearance" }` block used for the colour scheme, so
/// the whole appearance section lives in one place in the INI file.
constexpr auto kSettingsGroup = "appearance";
constexpr auto kSettingsKey = "language";

constexpr auto kEnglish = "en";
constexpr auto kChinese = "zh";

} // namespace

Translator::Translator(QObject* parent)
    : QObject(parent)
{
    // 1. The singleton is created in main() and handed to QML through
    //    create(). Declare C++ ownership so the engine does not delete it when
    //    the QML engine tears down.
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);

    // 2. Start in the saved language, or in the system one on first launch.
    //    English is the source language, so it needs no translator at all.
    m_language = initialLanguage();

    if (kChinese == m_language)
    {
        if (m_translator.load(QString::fromLatin1(kChineseCatalog)))
        {
            QCoreApplication::installTranslator(&m_translator);
        }
        else
        {
            qWarning("Translator: cannot load %s, staying in English.", kChineseCatalog);
            m_language = QString::fromLatin1(kEnglish);
        }
    }
}

Translator::~Translator()
{
    QCoreApplication::removeTranslator(&m_translator);
}

Translator* Translator::instance()
{
    // Intentionally leaked: a static object would be destroyed after
    // QCoreApplication, and its destructor touches the application's
    // translator list. One small object per process is the cheaper trade.
    static Translator* translator = new Translator();
    return translator;
}

Translator* Translator::create(QQmlEngine* engine, QJSEngine* jsEngine)
{
    Q_UNUSED(jsEngine);

    // 1. Reuse the instance main() already created and installed, so the first
    //    QML frame is translated rather than the first click.
    Translator* translator = instance();

    // 2. Remember the engine: retranslate() is the call that re-evaluates
    //    every qsTr() binding after the catalogue changes.
    if (nullptr != engine)
    {
        translator->m_engine = engine;
    }

    return translator;
}

QString Translator::language() const
{
    return m_language;
}

void Translator::setLanguage(const QString& language)
{
    // 1. Normalise and ignore no-op writes: QML may re-evaluate the binding
    //    that assigns the same value on unrelated property changes.
    const QString next = canonical(language);

    if (m_language == next)
    {
        return;
    }

    // 2. Swap the catalogue. This is what Qt uses for every qsTr() call from
    //    now on.
    applyLanguage(next);

    // 3. Persist the choice so the next launch starts in the same language.
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    settings.setValue(QString::fromLatin1(kSettingsKey), next);

    emit languageChanged();
}

QString Translator::canonical(const QString& language)
{
    const QString value = language.trimmed().toLower();

    if (value.startsWith(QStringLiteral("zh")))
    {
        return QString::fromLatin1(kChinese);
    }

    // English is the source language and the fallback for anything the client
    // does not ship a catalogue for.
    return QString::fromLatin1(kEnglish);
}

QString Translator::initialLanguage()
{
    // 1. An explicit choice always wins.
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    const QString saved = settings.value(QString::fromLatin1(kSettingsKey)).toString();

    if (!saved.isEmpty())
    {
        return canonical(saved);
    }

    // 2. First launch: follow the desktop locale, so a Chinese desktop opens
    //    in Chinese and everything else opens in English.
    return QLocale::system().language() == QLocale::Chinese
               ? QString::fromLatin1(kChinese)
               : QString::fromLatin1(kEnglish);
}

void Translator::applyLanguage(const QString& language)
{
    // 1. Drop the previous catalogue first; installing a second translator
    //    would leave the old strings shadowing the new ones.
    QCoreApplication::removeTranslator(&m_translator);

    // 2. English is the source language: with no translator installed, qsTr()
    //    returns the original strings.
    if (kChinese == language)
    {
        if (m_translator.load(QString::fromLatin1(kChineseCatalog)))
        {
            QCoreApplication::installTranslator(&m_translator);
        }
        else
        {
            // A missing catalogue must not brick the UI: report once and keep
            // the source language rather than half-translating.
            qWarning("Translator: cannot load %s, staying in English.", kChineseCatalog);
            m_language = QString::fromLatin1(kEnglish);
            return;
        }
    }

    m_language = language;

    // 3. Re-evaluate every qsTr() binding in the engine. Without this, only
    //    strings created after the switch would use the new catalogue.
    if (nullptr != m_engine)
    {
        m_engine->retranslate();
    }
}

} // namespace pulse::client