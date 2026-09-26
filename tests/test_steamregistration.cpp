// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>

#include "SessionManager.h"
#include "SteamConfigManager.h"
#include "SteamShortcutManager.h"
#include "SteamShortcutsVdf.h"

namespace {

class EnvironmentGuard
{
public:
    explicit EnvironmentGuard(const char *name)
        : m_name(name)
        , m_value(qgetenv(name))
        , m_wasSet(qEnvironmentVariableIsSet(name))
    {
    }

    ~EnvironmentGuard()
    {
        if (m_wasSet) {
            qputenv(m_name.constData(), m_value);
        } else {
            qunsetenv(m_name.constData());
        }
    }

private:
    QByteArray m_name;
    QByteArray m_value;
    bool m_wasSet = false;
};

} // namespace

class SteamRegistrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void registersProfileInSteamShortcuts()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());

        EnvironmentGuard homeGuard("HOME");
        EnvironmentGuard dataHomeGuard("XDG_DATA_HOME");
        EnvironmentGuard flatpakGuard("FLATPAK_ID");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/data")).toLocal8Bit());
        qputenv("FLATPAK_ID", QByteArrayLiteral("io.github.hikaps.couchplay"));
        QStandardPaths::setTestModeEnabled(true);

        const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
        const QString accountConfig = steamRoot + QStringLiteral("/userdata/123/config");
        QVERIFY(QDir().mkpath(accountConfig));
        QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
        QFile libraryFolders(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
        QVERIFY(libraryFolders.open(QIODevice::WriteOnly));
        const QByteArray libraryFoldersData = QByteArrayLiteral("\"libraryfolders\"\n{\n}\n");
        QCOMPARE(libraryFolders.write(libraryFoldersData), qint64(libraryFoldersData.size()));
        libraryFolders.close();

        const QString shortcutsPath = accountConfig + QStringLiteral("/shortcuts.vdf");
        QFile shortcuts(shortcutsPath);
        QVERIFY(shortcuts.open(QIODevice::WriteOnly));
        const QByteArray initial = SteamShortcutsVdf::emptyDocument();
        QCOMPARE(shortcuts.write(initial), qint64(initial.size()));
        shortcuts.close();

        SessionManager sessionManager;
        const QString profileName = QStringLiteral(R"(Party "Profile")");
        QVERIFY(sessionManager.saveProfile(profileName));

        SteamShortcutManager manager;
        manager.setSessionManager(&sessionManager);
        QSignalSpy prepared(&manager, &SteamShortcutManager::prepared);
        QSignalSpy finished(&manager, &SteamShortcutManager::registrationFinished);
        QSignalSpy errors(&manager, &SteamShortcutManager::errorOccurred);

        QVERIFY(manager.prepare(profileName));
        QCOMPARE(prepared.count(), 1);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(manager.accounts().size(), 1);

        manager.addToSteam(0);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(finished.at(0).at(0).toString(), profileName);
        QVERIFY(finished.at(0).at(1).toBool());

        QFile resultFile(shortcutsPath);
        QVERIFY(resultFile.open(QIODevice::ReadOnly));
        const QByteArray result = resultFile.readAll();
        QVERIFY(result != initial);

        QVERIFY(result.contains(QByteArrayLiteral("couchplay://profile/")));
        SteamConfigManager configManager;
        QCOMPARE(configManager.steamPaths().shortcutsVdf, shortcutsPath);
        configManager.loadShortcuts();
        const QVariantList registered = configManager.shortcutsAsVariant();
        QCOMPARE(registered.size(), 1);
        const QVariantMap shortcut = registered.constFirst().toMap();
        QCOMPARE(shortcut.value(QStringLiteral("appName")).toString(),
                 QStringLiteral("CouchPlay - ") + profileName);
        QCOMPARE(shortcut.value(QStringLiteral("exe")).toString(), QStringLiteral("\"/usr/bin/flatpak\""));
        const QString launchOptions = shortcut.value(QStringLiteral("launchOptions")).toString();
        const QString expectedOptions = QStringLiteral("run --command=couchplay-gamemode io.github.hikaps.couchplay --profile ")
            + QStringLiteral("\"Party \\\"Profile\\\"\"") + QStringLiteral(" --start --exit-after-session");
        QCOMPARE(launchOptions, expectedOptions);
        QVERIFY(QFileInfo::exists(accountConfig + QStringLiteral("/shortcuts.vdf.backup")));

        QStandardPaths::setTestModeEnabled(false);
    }
};

QTEST_MAIN(SteamRegistrationTest)
#include "test_steamregistration.moc"
