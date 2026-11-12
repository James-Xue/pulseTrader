#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "i18n/Translator.hpp"

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

QTEST_GUILESS_MAIN(TstTranslator)

#include "tst_translator.moc"