#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

#include <type_traits>

#include "i18n/Translator.hpp"

// Regression guard. Qt's QML singleton registration prefers a default
// constructor over the create() factory (qqmlprivate.h,
// singletonConstructionMode()). Adding one back would make QML build a second
// instance with no QQmlEngine: the toggle would swap the translator but never
// call retranslate(), so the visible UI would stop switching languages.
static_assert(!std::is_default_constructible_v<pulse::client::Translator>,
              "Translator must not be default-constructible: Qt would construct a second "
              "instance for QML instead of calling create()");

/// Interface-language switching.
///
/// The catalog is embedded in this test binary under :/i18n (see
/// tests/CMakeLists.txt), so the assertions cover the real .qm that ships in
/// the client rather than a stub. QSettings is redirected to a temporary
/// directory: the tests must never touch the user's real configuration.
class TstTranslator : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void startsInSavedLanguage();
    void switchesToChinese();
    void restartKeepsTheSavedChoice();
    void switchingBackRevivesSourceStrings();
    void normalisesLanguageVariants();
    void noOpWriteDoesNotNotify();
    void catalogMatchesTheTsFile();

private:
    QTemporaryDir m_settingsDir;
};

void TstTranslator::initTestCase()
{
    // 1. Isolate QSettings: an INI file inside a temporary directory, under an
    //    application name that never collides with the real client.
    QCoreApplication::setOrganizationName(QStringLiteral("pulseTrader-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("pulseTrader-tests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QVERIFY(m_settingsDir.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());

    // 2. Seed English so the first Translator::instance() is deterministic even
    //    on a machine whose system locale is Chinese.
    QSettings settings;
    settings.beginGroup(QStringLiteral("appearance"));
    settings.setValue(QStringLiteral("language"), QStringLiteral("en"));
    settings.sync();
}

void TstTranslator::startsInSavedLanguage()
{
    pulse::client::Translator* translator = pulse::client::Translator::instance();

    QCOMPARE(translator->language(), QStringLiteral("en"));
    // English is the source language: with no translator installed the string
    // comes back untouched.
    QCOMPARE(QCoreApplication::translate("StatusBar", "connected"), QStringLiteral("connected"));
}

void TstTranslator::switchesToChinese()
{
    pulse::client::Translator* translator = pulse::client::Translator::instance();
    QSignalSpy spy(translator, &pulse::client::Translator::languageChanged);

    translator->setLanguage(QStringLiteral("zh"));

    QCOMPARE(translator->language(), QStringLiteral("zh"));
    QCOMPARE(spy.count(), 1);

    // Both a QML context and a C++ context: the catalog covers qsTr() in the
    // QML layer and tr() in AccountService/EngineLauncher alike.
    QCOMPARE(QCoreApplication::translate("StatusBar", "connected"), QStringLiteral("已连接"));
    QCOMPARE(QCoreApplication::translate("pulse::client::AccountService",
                                         "The two passwords do not match."),
             QStringLiteral("两次输入的密码不一致。"));
}

void TstTranslator::restartKeepsTheSavedChoice()
{
    // The choice is persisted for the next process; a second instance built
    // from the same settings therefore starts in Chinese.
    QSettings settings;
    settings.beginGroup(QStringLiteral("appearance"));
    QCOMPARE(settings.value(QStringLiteral("language")).toString(), QStringLiteral("zh"));
}

void TstTranslator::switchingBackRevivesSourceStrings()
{
    pulse::client::Translator* translator = pulse::client::Translator::instance();

    translator->setLanguage(QStringLiteral("en"));

    QCOMPARE(translator->language(), QStringLiteral("en"));
    QCOMPARE(QCoreApplication::translate("StatusBar", "connected"), QStringLiteral("connected"));
}

void TstTranslator::normalisesLanguageVariants()
{
    pulse::client::Translator* translator = pulse::client::Translator::instance();

    // A locale-style value still selects the Chinese catalog.
    translator->setLanguage(QStringLiteral("zh_CN"));
    QCOMPARE(translator->language(), QStringLiteral("zh"));

    // Anything the client does not ship falls back to the source language
    // instead of leaving the UI half-translated.
    translator->setLanguage(QStringLiteral("fr"));
    QCOMPARE(translator->language(), QStringLiteral("en"));
}

void TstTranslator::noOpWriteDoesNotNotify()
{
    pulse::client::Translator* translator = pulse::client::Translator::instance();
    QSignalSpy spy(translator, &pulse::client::Translator::languageChanged);

    translator->setLanguage(translator->language());

    QCOMPARE(spy.count(), 0);
}

void TstTranslator::catalogMatchesTheTsFile()
{
    // 1. Read the .ts catalog — the source of truth — into (context, source,
    //    translation) triples. Entries still marked unfinished are skipped:
    //    they are allowed to fall back to the source string at runtime.
    QFile file(QStringLiteral(PULSE_TS_PATH));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));

    struct Entry
    {
        QString context;
        QString source;
        QString translation;
    };

    QList<Entry> entries;
    QString context;
    QXmlStreamReader xml(&file);

    while (!xml.atEnd())
    {
        xml.readNext();

        if (!xml.isStartElement())
        {
            continue;
        }

        if (xml.name() == u"name")
        {
            context = xml.readElementText();
            continue;
        }

        if (xml.name() != u"message")
        {
            continue;
        }

        QString source;
        QString translation;

        // readElementText() consumes the child's end tag, so this loop walks
        // <source>, <translation> and <comment> one element at a time.
        while (!xml.atEnd())
        {
            xml.readNext();

            if (xml.isEndElement() && xml.name() == u"message")
            {
                break;
            }

            if (!xml.isStartElement())
            {
                continue;
            }

            if (xml.name() == u"source")
            {
                source = xml.readElementText();
            }
            else if (xml.name() == u"translation")
            {
                const bool unfinished = xml.attributes().value("type") == u"unfinished";
                translation = xml.readElementText();

                if (unfinished)
                {
                    translation.clear();
                }
            }
        }

        if (!source.isEmpty() && !translation.isEmpty())
        {
            entries.append({ context, source, translation });
        }
    }

    QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));

    // 2. The catalog must be non-trivial; a truncated .ts would otherwise let
    //    this test pass with nothing to check.
    QVERIFY2(entries.size() >= 150,
             qPrintable(QStringLiteral("catalog has only %1 entries").arg(entries.size())));

    // 3. Every entry must come back from the committed .qm exactly as the .ts
    //    says. That .qm is what the LinguistTools-less fallback build embeds,
    //    and a stale one (edited .ts, forgotten lrelease) would otherwise ship
    //    silently: the default build regenerates from the .ts and never
    //    notices.
    QTranslator catalog;
    QVERIFY(catalog.load(QStringLiteral(":/i18n/pulsetrader_zh_CN.qm")));

    for (const Entry& entry : entries)
    {
        const QByteArray contextBytes = entry.context.toUtf8();
        const QByteArray sourceBytes = entry.source.toUtf8();

        QCOMPARE(catalog.translate(contextBytes.constData(), sourceBytes.constData()),
                 entry.translation);
    }
}

QTEST_GUILESS_MAIN(TstTranslator)

#include "tst_translator.moc"