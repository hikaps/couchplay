// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVariantList>

#include <functional>
#include <qqmlintegration.h>
#include <unistd.h>

#define private public
#include "SteamShortcutManager.h"
#undef private

#include "SessionManager.h"
class TestSteamShortcutManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCancelDuringShutdownPollingReopensSteam()
    {
        if (::geteuid() == 0) {
            QSKIP("host Steam script refuses to run as root");
        }

        struct RestoreEnvironment {
            QByteArray home = qgetenv("HOME");
            QByteArray dataHome = qgetenv("XDG_DATA_HOME");
            QByteArray gamescopeDisplay = qgetenv("GAMESCOPE_WAYLAND_DISPLAY");
            QByteArray steamGamepadUi = qgetenv("SteamGamepadUI");
            QByteArray desktop = qgetenv("XDG_CURRENT_DESKTOP");
            ~RestoreEnvironment()
            {
                if (home.isNull()) qunsetenv("HOME"); else qputenv("HOME", home);
                if (dataHome.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", dataHome);
                if (gamescopeDisplay.isNull()) qunsetenv("GAMESCOPE_WAYLAND_DISPLAY");
                else qputenv("GAMESCOPE_WAYLAND_DISPLAY", gamescopeDisplay);
                if (steamGamepadUi.isNull()) qunsetenv("SteamGamepadUI"); else qputenv("SteamGamepadUI", steamGamepadUi);
                if (desktop.isNull()) qunsetenv("XDG_CURRENT_DESKTOP"); else qputenv("XDG_CURRENT_DESKTOP", desktop);
                QStandardPaths::setTestModeEnabled(false);
            }
        } restoreEnvironment;

        QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_DATA_HOME", (home.path() + QStringLiteral("/.local/share")).toLocal8Bit());
        qunsetenv("GAMESCOPE_WAYLAND_DISPLAY");
        qunsetenv("SteamGamepadUI");
        qunsetenv("XDG_CURRENT_DESKTOP");
        QStandardPaths::setTestModeEnabled(true);

        const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
        const QString configDir = steamRoot + QStringLiteral("/config");
        const QString steamUserConfig = steamRoot + QStringLiteral("/userdata/123/config");
        const QString steamBinary = steamRoot + QStringLiteral("/ubuntu12_64/steam");
        QVERIFY(QDir().mkpath(configDir));
        QVERIFY(QDir().mkpath(steamUserConfig));
        QVERIFY(QDir().mkpath(QFileInfo(steamBinary).absolutePath()));
        QFile libraryFolders(configDir + QStringLiteral("/libraryfolders.vdf"));
        QVERIFY(libraryFolders.open(QIODevice::WriteOnly));
        const QByteArray libraryFoldersContents = "\"libraryfolders\" { }\n";
        QCOMPARE(libraryFolders.write(libraryFoldersContents), qint64(libraryFoldersContents.size()));
        libraryFolders.close();
        QVERIFY(QFile::copy(QStringLiteral("/bin/sleep"), steamBinary));
        QVERIFY(QFile::setPermissions(steamBinary, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        const QString steamScriptPath = steamRoot + QStringLiteral("/steam.sh");
        QFile steamScript(steamScriptPath);
        QVERIFY(steamScript.open(QIODevice::WriteOnly));
        const QByteArray steamScriptContents = "#!/bin/sh\nif [ \"$1\" = \"-shutdown\" ]; then exit 0; fi\nprintf reopened > \"$HOME/steam-reopened\"\n";
        QCOMPARE(steamScript.write(steamScriptContents), qint64(steamScriptContents.size()));
        steamScript.close();
        QVERIFY(QFile::setPermissions(steamScriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        SessionManager sessionManager;
        QVERIFY(sessionManager.saveProfile(QStringLiteral("Cancel Race Profile")));
        SteamShortcutManager manager;
        manager.setSessionManager(&sessionManager);
        manager.m_root = steamRoot;
        manager.m_accountId = QStringLiteral("123");
        manager.m_wasRunning = true;
        manager.m_shutdownConfirmed = false;
        manager.m_cancelled = false;
        manager.m_pollAttempts = 0;
        manager.m_phase = SteamShortcutManager::Phase::ClosingSteam;
        manager.m_busy = true;
        QSignalSpy registrationFinished(&manager, &SteamShortcutManager::registrationFinished);

        QProcess fakeSteam;
        fakeSteam.start(steamBinary, {QStringLiteral("30")});
        QVERIFY(fakeSteam.waitForStarted(3000));
        manager.pollSteamStopped();
        QTRY_VERIFY_WITH_TIMEOUT(manager.m_pollTimer && manager.m_pollTimer->isActive() && !manager.m_process, 5000);

        manager.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(manager.m_cancelled && manager.m_pollTimer && manager.m_pollTimer->isActive()
                                     && !manager.m_process,
                                 5000);
        fakeSteam.terminate();
        QVERIFY(fakeSteam.waitForFinished(3000));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(home.path() + QStringLiteral("/steam-reopened")) && !manager.busy(), 5000);

        QCOMPARE(registrationFinished.size(), 0);
        QCOMPARE(manager.m_phase, SteamShortcutManager::Phase::Idle);
        QVERIFY(!manager.m_cancelled);
    }

    void testInvalidProfileFailsBeforeHostProbe()
    {
        SteamShortcutManager manager;
        QSignalSpy errors(&manager, &SteamShortcutManager::errorOccurred);

        manager.prepare(QStringLiteral("../invalid"));

        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.constFirst().constFirst().toString(), QStringLiteral("Invalid saved profile"));
        QVERIFY(!manager.busy());
    }
};

QTEST_MAIN(TestSteamShortcutManager)
#include "test_steamshortcutmanager.moc"
