// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QByteArray>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVariantList>

#include <qqmlintegration.h>
#include <grp.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#define private public
#include "SteamShortcutManager.h"
#undef private

#include "SessionManager.h"

namespace {

constexpr qsizetype MaxHostDocumentSize = 16 * 1024 * 1024;

QString hostScript()
{
    QFile file(QStringLiteral(":/couchplay/steam-host.sh"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}
QString gameModeScript()
{
    QFile file(QStringLiteral(":/couchplay/gamemode.sh"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

class HostTestHome
{
private:
    QByteArray oldHome = qgetenv("HOME");
    QByteArray oldDataHome = qgetenv("XDG_DATA_HOME");
    QByteArray oldGamescope = qgetenv("GAMESCOPE_WAYLAND_DISPLAY");
    QByteArray oldGamepadUi = qgetenv("SteamGamepadUI");
    QByteArray oldDesktop = qgetenv("XDG_CURRENT_DESKTOP");

    static void restore(const char *name, const QByteArray &value)
    {
        if (value.isNull()) {
            qunsetenv(name);
        } else {
            qputenv(name, value);
        }
    }

public:
    HostTestHome()
    {
        if (!home.isValid()) {
            return;
        }
        dataHome = home.path() + QStringLiteral("/data");
        steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
        configDir = steamRoot + QStringLiteral("/userdata/123/config");
        shortcutsPath = configDir + QStringLiteral("/shortcuts.vdf");
        qputenv("HOME", home.path().toLocal8Bit());
        qputenv("XDG_DATA_HOME", dataHome.toLocal8Bit());
        qunsetenv("GAMESCOPE_WAYLAND_DISPLAY");
        qunsetenv("SteamGamepadUI");
        qunsetenv("XDG_CURRENT_DESKTOP");
        if (!QDir().mkpath(steamRoot + QStringLiteral("/config"))
            || !QDir().mkpath(configDir) || !QDir().mkpath(dataHome)) {
            return;
        }
        QFile libraryFolders(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
        if (!libraryFolders.open(QIODevice::WriteOnly)) {
            return;
        }
        const QByteArray contents = "\"libraryfolders\" { }\n";
        ready = libraryFolders.write(contents) == contents.size();
        if (ready && ::geteuid() == 0) {
            const QByteArray homePath = home.path().toLocal8Bit();
            const QByteArray dataPath = dataHome.toLocal8Bit();
            const QByteArray configPath = configDir.toLocal8Bit();
            ready = ::chmod(homePath.constData(), 0777) == 0
                && ::chown(dataPath.constData(), 65534, 65534) == 0
                && ::chmod(configPath.constData(), 0777) == 0;
        }
    }

    ~HostTestHome()
    {
        restore("HOME", oldHome);
        restore("XDG_DATA_HOME", oldDataHome);
        restore("GAMESCOPE_WAYLAND_DISPLAY", oldGamescope);
        restore("SteamGamepadUI", oldGamepadUi);
        restore("XDG_CURRENT_DESKTOP", oldDesktop);
    }

    void configure(QProcess &process, const QString &operation, const QStringList &arguments = {}) const
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("HOME"), home.path());
        environment.insert(QStringLiteral("XDG_DATA_HOME"), dataHome);
        environment.remove(QStringLiteral("GAMESCOPE_WAYLAND_DISPLAY"));
        environment.remove(QStringLiteral("SteamGamepadUI"));
        environment.remove(QStringLiteral("XDG_CURRENT_DESKTOP"));
        environment.remove(QStringLiteral("FLATPAK_ID"));
        process.setProcessEnvironment(environment);
        process.setProgram(QStringLiteral("/bin/bash"));
        process.setChildProcessModifier([] {
            if (::setpgid(0, 0) != 0
                || (::geteuid() == 0 && (::setgroups(0, nullptr) != 0 || ::setgid(65534) != 0 || ::setuid(65534) != 0))) {
                ::_exit(127);
            }
        });
        QStringList processArguments{QStringLiteral("-c"), hostScript(),
                                     QStringLiteral("couchplay-steam-host"), operation};
        processArguments.append(arguments);
        process.setArguments(processArguments);
    }
    bool makeShortcutHostOwned() const
    {
        if (::geteuid() != 0) {
            return true;
        }
        const QByteArray path = shortcutsPath.toLocal8Bit();
        return ::chown(path.constData(), 65534, 65534) == 0;
    }

    QTemporaryDir home;
    QString dataHome;
    QString steamRoot;
    QString configDir;
    QString shortcutsPath;
    bool ready = false;
};

QString fileHash(const QByteArray &contents)
{
    return QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

bool hasTemporaryFiles(const QString &directory, const QString &pattern)
{
    return !QDir(directory).entryList({pattern}, QDir::Files | QDir::Hidden).isEmpty();
}

bool writeFile(const QString &path, const QByteArray &contents, QFileDevice::Permissions permissions = {})
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(contents) != contents.size()) {
        return false;
    }
    file.close();
    return permissions == QFileDevice::Permissions() || QFile::setPermissions(path, permissions);
}

} // namespace

class TestSteamShortcutManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCancelDuringShutdownPollingReopensSteam()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host Steam detection requires Linux procfs");
        }
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
        const QByteArray steamScriptContents = "#!/bin/sh\nif [ \"$1\" = \"-shutdown\" ]; then exit 0; fi\nprintf reopened > \"$HOME/steam-reopened\"\n\"$HOME/.local/share/Steam/ubuntu12_64/steam\" 30 &\necho $! > \"$HOME/steam-pid\"\nwait\n";
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
        manager.m_shutdownDeadline = QDeadlineTimer(30000, Qt::PreciseTimer);
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

        QFile pidFile(home.path() + QStringLiteral("/steam-pid"));
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        bool pidOk = false;
        const pid_t restartedPid = static_cast<pid_t>(pidFile.readAll().trimmed().toLongLong(&pidOk));
        QVERIFY(pidOk && restartedPid > 0);
        QCOMPARE(::kill(restartedPid, SIGTERM), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(QStringLiteral("/proc/%1/exe").arg(restartedPid)), 3000);
        QCOMPARE(registrationFinished.size(), 0);
        QCOMPARE(manager.m_phase, SteamShortcutManager::Phase::Idle);
        QVERIFY(!manager.m_cancelled);
    }

    void testCancellationDeadlineDoesNotAssumeSteamStopped()
    {
        SteamShortcutManager manager;
        manager.m_wasRunning = true;
        manager.m_root = QStringLiteral("/unused-steam-root");
        manager.m_shutdownConfirmed = false;
        manager.m_cancelled = true;
        manager.m_phase = SteamShortcutManager::Phase::ClosingSteam;
        manager.m_busy = true;
        manager.m_shutdownDeadline = QDeadlineTimer(500, Qt::PreciseTimer);
        QSignalSpy errors(&manager, &SteamShortcutManager::errorOccurred);
        QSignalSpy registrationFinished(&manager, &SteamShortcutManager::registrationFinished);

        manager.pollSteamStopped();

        QVERIFY(!manager.busy());
        QVERIFY(!manager.m_shutdownConfirmed);
        QVERIFY(!manager.m_process);
        QVERIFY(!manager.m_pollTimer);
        QCOMPARE(manager.m_phase, SteamShortcutManager::Phase::Idle);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.constFirst().constFirst().toString().contains(QStringLiteral("may still be running")));
        QCOMPARE(registrationFinished.size(), 0);
    }

    void testFailedRecoveryIsReportedAfterRegistrationFailure()
    {
        SteamShortcutManager manager;
        manager.m_root = QStringLiteral("/invalid-steam-root");
        manager.m_wasRunning = true;
        manager.m_shutdownConfirmed = true;
        QSignalSpy errors(&manager, &SteamShortcutManager::errorOccurred);

        manager.fail(QStringLiteral("Shortcut export failed"));

        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 5000);
        const QString message = errors.constFirst().constFirst().toString();
        QVERIFY(message.contains(QStringLiteral("Shortcut export failed")));
        QVERIFY(message.contains(QStringLiteral("could not be reopened")));
        QCOMPARE(manager.status(), message);
        QVERIFY(!manager.busy());
    }

    void testFailedRecoveryIsReportedAfterSuccessfulRegistration()
    {
        SteamShortcutManager manager;
        manager.m_root = QStringLiteral("/invalid-steam-root");
        manager.m_wasRunning = true;
        QSignalSpy errors(&manager, &SteamShortcutManager::errorOccurred);
        QSignalSpy finished(&manager, &SteamShortcutManager::registrationFinished);

        manager.reopenSteam(true, true);

        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 5000);
        QVERIFY(manager.status().contains(QStringLiteral("could not be reopened")));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.constFirst().at(2).toBool());
        QVERIFY(!manager.busy());
    }

    void testCancellationKillsDescendantsAfterProcessLeaderExits()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("process-group cleanup requires Linux procfs");
        }

        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString childPidPath = home.path() + QStringLiteral("/child.pid");
        QProcess process;
        process.setProgram(QStringLiteral("/bin/bash"));
        process.setArguments({QStringLiteral("-c"),
                              QStringLiteral("(trap '' TERM; echo $BASHPID > \"$1\"; exec /bin/sleep 30 >/dev/null 2>&1) & wait"),
                              QStringLiteral("test-process"), childPidPath});
        process.setChildProcessModifier([] {
            if (::setpgid(0, 0) != 0) {
                ::_exit(127);
            }
        });
        process.start();
        QVERIFY(process.waitForStarted(3000));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(childPidPath), 3000);

        bool ok = false;
        QFile childPidFile(childPidPath);
        QVERIFY(childPidFile.open(QIODevice::ReadOnly));
        const pid_t childPid = static_cast<pid_t>(childPidFile.readAll().trimmed().toLongLong(&ok));
        QVERIFY(ok && childPid > 0);

        SteamShortcutManager manager;
        manager.m_process = &process;
        manager.m_hostProcessGroupId = process.processId();
        manager.m_phase = SteamShortcutManager::Phase::Probing;
        manager.m_busy = true;
        manager.cancel();

        QVERIFY(process.waitForFinished(3000));
        manager.m_process = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(QStringLiteral("/proc/%1/exe").arg(childPid)), 5000);
    }

    void testDestroyingManagerCancelsDelayedHostSideEffect()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host process supervision requires Linux procfs");
        }
        if (::geteuid() == 0) {
            QSKIP("host Steam script refuses to run as root");
        }
        HostTestHome home;
        QVERIFY(home.ready);
        QVERIFY(QFile::remove(home.steamRoot + QStringLiteral("/config/libraryfolders.vdf")));

        const QString flatpakRoot = home.home.path()
            + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam");
        QVERIFY(QDir().mkpath(flatpakRoot + QStringLiteral("/config")));
        QVERIFY(writeFile(flatpakRoot + QStringLiteral("/config/libraryfolders.vdf"),
                          "\"libraryfolders\" { }\n"));

        const QString binDir = home.home.path() + QStringLiteral("/bin");
        QVERIFY(QDir().mkpath(binDir));
        QVERIFY(writeFile(binDir + QStringLiteral("/flatpak"),
                          "#!/bin/bash\n"
                          "[[ \"$1\" == ps ]] || exit 1\n"
                          "( trap '' TERM; : > \"$HOME/flatpak-started\"; /bin/sleep 2; : > \"$HOME/flatpak-side-effect\" ) &\n"
                          "wait\n",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", (binDir + QLatin1Char(':') + QString::fromLocal8Bit(oldPath)).toLocal8Bit());
        struct RestorePath {
            QByteArray value;
            ~RestorePath()
            {
                if (value.isNull()) {
                    qunsetenv("PATH");
                } else {
                    qputenv("PATH", value);
                }
            }
        } restorePath{oldPath};

        {
            SteamShortcutManager manager;
            manager.m_busy = true;
            manager.m_phase = SteamShortcutManager::Phase::Probing;
            manager.runHost(QStringLiteral("probe"), {}, QByteArray(),
                            [](int, const QByteArray &, const QString &) {}, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(home.home.path() + QStringLiteral("/flatpak-started")), 3000);
        }

        QTest::qWait(2500);
        QVERIFY(!QFile::exists(home.home.path() + QStringLiteral("/flatpak-side-effect")));
    }

    void testUnknownSteamProcessStatePreservesShortcuts()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host operations require Linux procfs");
        }

        {
            HostTestHome home;
            QVERIFY(home.ready);
            const QString root = home.home.path()
                + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam");
            const QString configDir = root + QStringLiteral("/userdata/123/config");
            QVERIFY(QDir().mkpath(root + QStringLiteral("/config")));
            QVERIFY(QDir().mkpath(configDir));
            QVERIFY(writeFile(root + QStringLiteral("/config/libraryfolders.vdf"),
                              "\"libraryfolders\" { }\n"));
            const QString shortcutsPath = configDir + QStringLiteral("/shortcuts.vdf");
            const QByteArray original = "existing flatpak shortcuts";
            QVERIFY(writeFile(shortcutsPath, original));
            if (::geteuid() == 0) {
                const QByteArray configPathBytes = configDir.toLocal8Bit();
                const QByteArray shortcutPathBytes = shortcutsPath.toLocal8Bit();
                QVERIFY(::chown(configPathBytes.constData(), 65534, 65534) == 0);
                QVERIFY(::chmod(configPathBytes.constData(), 0777) == 0);
                QVERIFY(::chown(shortcutPathBytes.constData(), 65534, 65534) == 0);
            }

            const QString binDir = home.home.path() + QStringLiteral("/bin");
            QVERIFY(QDir().mkpath(binDir));
            QVERIFY(writeFile(binDir + QStringLiteral("/flatpak"), "#!/bin/sh\nexit 1\n",
                              QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther
                                  | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther));
            QProcess process;
            home.configure(process, QStringLiteral("commit"), {root, QStringLiteral("123"), fileHash(original)});
            QProcessEnvironment environment = process.processEnvironment();
            environment.insert(QStringLiteral("PATH"),
                               binDir + QLatin1Char(':') + environment.value(QStringLiteral("PATH")));
            process.setProcessEnvironment(environment);
            process.start();
            QVERIFY(process.waitForStarted(3000));
            QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitCode(), 3);
            QVERIFY(process.readAllStandardError().contains("steam-process-state-unknown"));
            QFile shortcuts(shortcutsPath);
            QVERIFY(shortcuts.open(QIODevice::ReadOnly));
            QCOMPARE(shortcuts.readAll(), original);
            QVERIFY(!hasTemporaryFiles(configDir, QStringLiteral(".shortcuts.vdf.couchplay.*")));
        }

        {
            HostTestHome home;
            QVERIFY(home.ready);
            const QByteArray original = "existing native shortcuts";
            QVERIFY(writeFile(home.shortcutsPath, original));
            QVERIFY(home.makeShortcutHostOwned());
            const QString binary = home.steamRoot + QStringLiteral("/ubuntu12_64/steam");
            QVERIFY(QDir().mkpath(QFileInfo(binary).absolutePath()));
            QVERIFY(QFile::copy(QStringLiteral("/bin/sleep"), binary));
            QVERIFY(QFile::setPermissions(binary, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                    | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                                    | QFileDevice::ReadOther | QFileDevice::ExeOther));

            const QString binDir = home.home.path() + QStringLiteral("/bin");
            QVERIFY(QDir().mkpath(binDir));
            QVERIFY(writeFile(binDir + QStringLiteral("/readlink"),
                              "#!/bin/sh\ncase \" $* \" in *\"/proc/$COUCHPLAY_UNINSPECTABLE_PID/exe\"*) exit 1;; esac\nexec /usr/bin/readlink \"$@\"\n",
                              QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther
                                  | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther));
            QProcess fakeSteam;
            fakeSteam.setProgram(binary);
            fakeSteam.setArguments({QStringLiteral("30")});
            fakeSteam.setChildProcessModifier([] {
                if (::setpgid(0, 0) != 0
                    || (::geteuid() == 0 && (::setgroups(0, nullptr) != 0 || ::setgid(65534) != 0 || ::setuid(65534) != 0))) {
                    ::_exit(127);
                }
            });
            fakeSteam.start();
            QVERIFY(fakeSteam.waitForStarted(3000));
            QProcess process;
            home.configure(process, QStringLiteral("commit"),
                           {home.steamRoot, QStringLiteral("123"), fileHash(original)});
            QProcessEnvironment environment = process.processEnvironment();
            environment.insert(QStringLiteral("PATH"),
                               binDir + QLatin1Char(':') + environment.value(QStringLiteral("PATH")));
            environment.insert(QStringLiteral("COUCHPLAY_UNINSPECTABLE_PID"),
                               QString::number(fakeSteam.processId()));
            process.setProcessEnvironment(environment);
            process.start();
            QVERIFY(process.waitForStarted(3000));
            QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitCode(), 3);
            QVERIFY(process.readAllStandardError().contains("steam-process-state-unknown"));
            fakeSteam.terminate();
            QVERIFY(fakeSteam.waitForFinished(3000));
            QFile shortcuts(home.shortcutsPath);
            QVERIFY(shortcuts.open(QIODevice::ReadOnly));
            QCOMPARE(shortcuts.readAll(), original);
            QVERIFY(!hasTemporaryFiles(home.configDir, QStringLiteral(".shortcuts.vdf.couchplay.*")));
        }
    }

    void testHostExitTrapCleansTrackedTemporaryFileOnTerm()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host signal cleanup requires Linux");
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString temporaryPath = directory.path() + QStringLiteral("/in-progress");
        QVERIFY(writeFile(temporaryPath, "partial"));

        QProcess process;
        process.setProgram(QStringLiteral("/bin/bash"));
        process.setArguments({QStringLiteral("-c"),
                              hostScript() + QStringLiteral("\nACTIVE_TEMP_FILE=\"$1\"\nkill -TERM \"$$\"\n"),
                              QStringLiteral("couchplay-cleanup-test"), temporaryPath});
        process.start();
        QVERIFY(process.waitForStarted(3000));
        QVERIFY(process.waitForFinished(3000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 143);
        QVERIFY(!QFile::exists(temporaryPath));
    }

    void testHostReadRejectsHardlinksAndOpensWithoutFollowingSymlinkRaces()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host operations require Linux procfs");
        }
        // HostTestHome drops the host-script child to an unprivileged account.
        HostTestHome home;
        QVERIFY(home.ready);

        const QString hardlinkSource = home.configDir + QStringLiteral("/shortcuts-source.vdf");
        QVERIFY(writeFile(hardlinkSource, "shortcuts"));
        const QByteArray sourcePath = hardlinkSource.toLocal8Bit();
        const QByteArray shortcutsPath = home.shortcutsPath.toLocal8Bit();
        QCOMPARE(::link(sourcePath.constData(), shortcutsPath.constData()), 0);
        QVERIFY(home.makeShortcutHostOwned());

        QProcess hardlinkRead;
        home.configure(hardlinkRead, QStringLiteral("read"), {home.steamRoot, QStringLiteral("123")});
        hardlinkRead.start();
        QVERIFY(hardlinkRead.waitForStarted(3000));
        QVERIFY(hardlinkRead.waitForFinished(5000));
        QCOMPARE(hardlinkRead.exitCode(), 3);
        QVERIFY(hardlinkRead.readAllStandardError().contains("unsafe-shortcuts-file"));
        QCOMPARE(hardlinkRead.readAllStandardOutput(), QByteArray());

        QVERIFY(QFile::remove(home.shortcutsPath));
        QVERIFY(writeFile(home.shortcutsPath, "trusted shortcuts"));
        QVERIFY(home.makeShortcutHostOwned());
        const QString outsidePath = home.home.path() + QStringLiteral("/outside.vdf");
        QVERIFY(writeFile(outsidePath, "secret outside file"));
        const QString binDir = home.home.path() + QStringLiteral("/bin");
        QVERIFY(QDir().mkpath(binDir));
        const QString pythonShim = binDir + QStringLiteral("/python3");
        QVERIFY(writeFile(pythonShim,
                          "#!/bin/sh\nrm -f -- \"$COUCHPLAY_RACE_PATH\"\n"
                          "ln -s -- \"$COUCHPLAY_RACE_TARGET\" \"$COUCHPLAY_RACE_PATH\" || exit 98\n"
                          "exec \"$COUCHPLAY_REAL_PYTHON\" \"$@\"\n",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                              | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
                              | QFileDevice::ExeOther));
        QProcess symlinkRaceRead;
        home.configure(symlinkRaceRead, QStringLiteral("read"), {home.steamRoot, QStringLiteral("123")});
        QProcessEnvironment environment = symlinkRaceRead.processEnvironment();
        environment.insert(QStringLiteral("PATH"), binDir + QLatin1Char(':') + qEnvironmentVariable("PATH"));
        const QString realPython = QStandardPaths::findExecutable(QStringLiteral("python3"));
        QVERIFY(!realPython.isEmpty());
        environment.insert(QStringLiteral("COUCHPLAY_REAL_PYTHON"), realPython);
        environment.insert(QStringLiteral("COUCHPLAY_RACE_PATH"), home.shortcutsPath);
        environment.insert(QStringLiteral("COUCHPLAY_RACE_TARGET"), outsidePath);
        symlinkRaceRead.setProcessEnvironment(environment);
        symlinkRaceRead.start();
        QVERIFY(symlinkRaceRead.waitForStarted(3000));
        QVERIFY(symlinkRaceRead.waitForFinished(5000));
        QCOMPARE(symlinkRaceRead.exitCode(), 3);
        QVERIFY(symlinkRaceRead.readAllStandardError().contains("unsafe-shortcuts-file"));
        QCOMPARE(symlinkRaceRead.readAllStandardOutput(), QByteArray());
        QVERIFY(QFileInfo(home.shortcutsPath).isSymLink());
    }

    void testHostReadBoundsDescriptorReadsAtMaximumDocumentSize()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host operations require Linux procfs");
        }
        // HostTestHome drops the host-script child to an unprivileged account.
        HostTestHome home;
        QVERIFY(home.ready);

        const QByteArray maximum(MaxHostDocumentSize, 'x');
        QVERIFY(writeFile(home.shortcutsPath, maximum));
        QVERIFY(home.makeShortcutHostOwned());
        QProcess maximumRead;
        home.configure(maximumRead, QStringLiteral("read"), {home.steamRoot, QStringLiteral("123")});
        maximumRead.start();
        QVERIFY(maximumRead.waitForStarted(3000));
        QVERIFY(maximumRead.waitForFinished(10000));
        QCOMPARE(maximumRead.exitCode(), 0);
        QCOMPARE(maximumRead.readAllStandardOutput(), maximum);

        QVERIFY(writeFile(home.shortcutsPath, maximum + QByteArray(1, 'x')));
        QVERIFY(home.makeShortcutHostOwned());
        QProcess oversizedRead;
        home.configure(oversizedRead, QStringLiteral("read"), {home.steamRoot, QStringLiteral("123")});
        oversizedRead.start();
        QVERIFY(oversizedRead.waitForStarted(3000));
        QVERIFY(oversizedRead.waitForFinished(5000));
        QCOMPARE(oversizedRead.exitCode(), 3);
        QVERIFY(oversizedRead.readAllStandardError().contains("shortcuts-file-too-large"));
        QVERIFY(oversizedRead.readAllStandardOutput().size() <= MaxHostDocumentSize + 1);
    }

    void testHostReadDetectsConcurrentMutationAndCapsDescriptorReads()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host operations require Linux procfs");
        }
        // HostTestHome drops the host-script child to an unprivileged account.
        HostTestHome home;
        QVERIFY(home.ready);
        QVERIFY(writeFile(home.shortcutsPath, "x"));
        QVERIFY(home.makeShortcutHostOwned());

        const QString hookDir = home.home.path() + QStringLiteral("/python-hook");
        QVERIFY(QDir().mkpath(hookDir));
        const QByteArray hook =
            "import atexit, os\n"
            "_count = 0\n_open_count = 0\n_done = False\n_original_read = os.read\n_original_open = os.open\n"
            "def _save_count():\n"
            "    with open(os.environ[\"COUCHPLAY_READ_COUNT\"], \"w\") as output: output.write(f\"{_open_count}:{_count}\")\n"
            "atexit.register(_save_count)\n"
            "def _open(path, flags, *args, **kwargs):\n"
            "    global _open_count\n"
            "    if os.fspath(path) == os.environ[\"COUCHPLAY_READ_PATH\"]: _open_count += 1\n"
            "    return _original_open(path, flags, *args, **kwargs)\n"
            "os.open = _open\n"
            "def _read(fd, size):\n"
            "    global _count, _done\n"
            "    if not _done:\n"
            "        _done = True\n"
            "        with open(os.environ[\"COUCHPLAY_READ_PATH\"], \"r+b\", buffering=0) as target:\n"
            "            target.truncate(0)\n"
            "            target.write(b\"x\" * (int(os.environ[\"COUCHPLAY_READ_LIMIT\"]) + 100))\n"
            "    data = _original_read(fd, size)\n"
            "    _count += len(data)\n"
            "    return data\n"
            "os.read = _read\n";
        QVERIFY(writeFile(hookDir + QStringLiteral("/sitecustomize.py"), hook));
        const QString readCountPath = home.home.path() + QStringLiteral("/read-count");

        QProcess process;
        home.configure(process, QStringLiteral("read"), {home.steamRoot, QStringLiteral("123")});
        QProcessEnvironment environment = process.processEnvironment();
        environment.insert(QStringLiteral("PYTHONPATH"), hookDir);
        environment.insert(QStringLiteral("COUCHPLAY_READ_PATH"), home.shortcutsPath);
        environment.insert(QStringLiteral("COUCHPLAY_READ_COUNT"), readCountPath);
        environment.insert(QStringLiteral("COUCHPLAY_READ_LIMIT"), QString::number(MaxHostDocumentSize));
        process.setProcessEnvironment(environment);
        process.start();
        QVERIFY(process.waitForStarted(3000));
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.exitCode(), 3);
        QVERIFY(process.readAllStandardError().contains("shortcuts-file-changed"));
        QCOMPARE(process.readAllStandardOutput(), QByteArray());

        QFile readCount(readCountPath);
        QVERIFY(readCount.open(QIODevice::ReadOnly));
        QCOMPARE(readCount.readAll().trimmed(), QByteArray("1:") + QByteArray::number(qint64(MaxHostDocumentSize + 1)));
    }

    void testHostSupervisorKillsTermIgnoringDescendantsOnTimeout()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host process supervision requires Linux procfs");
        }
        if (::geteuid() == 0) {
            QSKIP("host Steam script refuses to run as root");
        }
        HostTestHome home;
        QVERIFY(home.ready);
        const QString binDir = home.home.path() + QStringLiteral("/bin");
        QVERIFY(QDir().mkpath(binDir));
        const QString realpathShim = binDir + QStringLiteral("/realpath");
        QVERIFY(writeFile(realpathShim,
                          "#!/bin/bash\n( trap '' TERM; echo $BASHPID > \"$HOME/ignored.pid\"; exec /bin/sleep 30 >/dev/null 2>&1 ) &\n"
                          "/usr/bin/realpath \"$@\"\n/bin/sleep 30\n",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", (binDir + QLatin1Char(':') + QString::fromLocal8Bit(oldPath)).toLocal8Bit());
        struct RestorePath {
            QByteArray value;
            ~RestorePath()
            {
                if (value.isNull()) {
                    qunsetenv("PATH");
                } else {
                    qputenv("PATH", value);
                }
            }
        } restorePath{oldPath};

        SteamShortcutManager manager;
        manager.m_busy = true;
        manager.m_phase = SteamShortcutManager::Phase::Probing;
        QElapsedTimer elapsed;
        elapsed.start();
        bool completed = false;
        int exitCode = 0;
        qint64 completionTimeMs = 0;
        QString error;
        manager.runHost(QStringLiteral("probe"), {}, QByteArray(),
                        [&](int code, const QByteArray &, const QString &message) {
                            completed = true;
                            completionTimeMs = elapsed.elapsed();
                            exitCode = code;
                            error = message;
                        },
                        1000);

        const QString childPidPath = home.home.path() + QStringLiteral("/ignored.pid");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(childPidPath), 3000);
        QFile childPidFile(childPidPath);
        QVERIFY(childPidFile.open(QIODevice::ReadOnly));
        bool ok = false;
        const pid_t childPid = static_cast<pid_t>(childPidFile.readAll().trimmed().toLongLong(&ok));
        QVERIFY(ok && childPid > 0);
        QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
        QCOMPARE(exitCode, -1);
        QVERIFY(completionTimeMs >= 1400);
        QVERIFY(error.contains(QStringLiteral("timed out")));
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(QStringLiteral("/proc/%1/exe").arg(childPid)), 5000);
    }

    void testHostSupervisorHandlesFlatpakWatchBusInterrupt()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host process supervision requires Linux procfs");
        }
        if (::geteuid() == 0) {
            QSKIP("host Steam script refuses to run as root");
        }
        HostTestHome home;
        QVERIFY(home.ready);
        const QString binDir = home.home.path() + QStringLiteral("/bin");
        QVERIFY(QDir().mkpath(binDir));
        const QString realpathShim = binDir + QStringLiteral("/realpath");
        QVERIFY(writeFile(realpathShim,
                          "#!/bin/bash\n( trap '' TERM; echo $BASHPID > \"$HOME/interrupted.pid\"; exec /bin/sleep 30 >/dev/null 2>&1 ) &\n"
                          "/usr/bin/realpath \"$@\"\n/bin/sleep 30\n",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", (binDir + QLatin1Char(':') + QString::fromLocal8Bit(oldPath)).toLocal8Bit());
        struct RestorePath {
            QByteArray value;
            ~RestorePath()
            {
                if (value.isNull()) {
                    qunsetenv("PATH");
                } else {
                    qputenv("PATH", value);
                }
            }
        } restorePath{oldPath};

        SteamShortcutManager manager;
        manager.m_busy = true;
        manager.m_phase = SteamShortcutManager::Phase::Probing;
        bool completed = false;
        int exitCode = -1;
        manager.runHost(QStringLiteral("probe"), {}, QByteArray(),
                        [&](int code, const QByteArray &, const QString &) {
                            completed = true;
                            exitCode = code;
                        },
                        10000);

        const QString childPidPath = home.home.path() + QStringLiteral("/interrupted.pid");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(childPidPath), 3000);
        const pid_t processGroupId = static_cast<pid_t>(manager.m_process->processId());
        QVERIFY(processGroupId > 0);
        QCOMPARE(::kill(-processGroupId, SIGINT), 0);

        QFile childPidFile(childPidPath);
        QVERIFY(childPidFile.open(QIODevice::ReadOnly));
        bool ok = false;
        const pid_t childPid = static_cast<pid_t>(childPidFile.readAll().trimmed().toLongLong(&ok));
        QVERIFY(ok && childPid > 0);
        QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
        QCOMPARE(exitCode, 143);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(QStringLiteral("/proc/%1/exe").arg(childPid)), 5000);
    }

    void testHostRejectsOversizedCommitAndExportInput()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host operations require Linux procfs");
        }
        HostTestHome home;
        QVERIFY(home.ready);
        QVERIFY(!hostScript().isEmpty());
        const QByteArray oversized(MaxHostDocumentSize + 1, 'x');

        QProcess commit;
        home.configure(commit, QStringLiteral("commit"),
                       {home.steamRoot, QStringLiteral("123"), QStringLiteral("missing")});
        commit.start();
        QVERIFY(commit.waitForStarted(3000));
        QCOMPARE(commit.write(oversized), qint64(oversized.size()));
        commit.closeWriteChannel();
        QVERIFY(commit.waitForFinished(15000));
        QCOMPARE(commit.exitCode(), 4);
        QVERIFY(commit.readAllStandardError().contains("shortcuts-file-too-large"));
        QVERIFY(!QFile::exists(home.shortcutsPath));
        QVERIFY(!hasTemporaryFiles(home.configDir, QStringLiteral(".shortcuts.vdf.couchplay.*")));

        QProcess exportProcess;
        home.configure(exportProcess, QStringLiteral("export"),
                       {QString(64, QLatin1Char('a')), QStringLiteral("launcher")});
        exportProcess.start();
        QVERIFY(exportProcess.waitForStarted(3000));
        QCOMPARE(exportProcess.write(oversized), qint64(oversized.size()));
        exportProcess.closeWriteChannel();
        QVERIFY(exportProcess.waitForFinished(15000));
        QCOMPARE(exportProcess.exitCode(), 3);
        QVERIFY(exportProcess.readAllStandardError().contains("export-payload-too-large"));
        QVERIFY(!QFile::exists(home.dataHome + QStringLiteral("/couchplay/steam-shortcuts/")
                             + QString(64, QLatin1Char('a')) + QStringLiteral(".sh")));
        QVERIFY(!hasTemporaryFiles(home.dataHome + QStringLiteral("/couchplay/steam-shortcuts"),
                                   QStringLiteral(".export.*")));
    }

    void testCommitRejectsConcurrentShortcutChangeBeforeRename()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host operations require Linux procfs");
        }
        HostTestHome home;
        QVERIFY(home.ready);
        const QByteArray original = "original shortcuts";
        const QByteArray concurrent = "concurrent Steam update";
        QVERIFY(writeFile(home.shortcutsPath, original));
        QVERIFY(home.makeShortcutHostOwned());

        QProcess process;
        home.configure(process, QStringLiteral("commit"),
                       {home.steamRoot, QStringLiteral("123"), fileHash(original)});
        process.start();
        QVERIFY(process.waitForStarted(3000));
        QVERIFY(process.write("replacement shortcuts") > 0);
        QTRY_VERIFY_WITH_TIMEOUT(hasTemporaryFiles(home.configDir, QStringLiteral(".shortcuts.vdf.couchplay.*")), 5000);
        QVERIFY(writeFile(home.shortcutsPath, concurrent));
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(5000));

        QCOMPARE(process.exitCode(), 4);
        QVERIFY(process.readAllStandardError().contains("shortcuts-conflict"));
        QFile shortcuts(home.shortcutsPath);
        QVERIFY(shortcuts.open(QIODevice::ReadOnly));
        QCOMPARE(shortcuts.readAll(), concurrent);
        QVERIFY(!hasTemporaryFiles(home.configDir, QStringLiteral(".shortcuts.vdf.couchplay.*")));
    }

    void testHostExitTrapCleansTemporaryFileOnTermination()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host cleanup test requires Linux procfs");
        }
        HostTestHome home;
        QVERIFY(home.ready);

        QProcess process;
        home.configure(process, QStringLiteral("probe"));
        QStringList arguments = process.arguments();
        arguments[1] = hostScript() + QStringLiteral("\nACTIVE_TEMP_FILE=$(mktemp -- \"$HOME/.couchplay-timeout.XXXXXX\")\nsleep 30\n");
        process.setArguments(arguments);
        process.start();
        QVERIFY(process.waitForStarted(3000));
        process.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(hasTemporaryFiles(home.home.path(), QStringLiteral(".couchplay-timeout.*")), 5000);

        const pid_t processGroup = static_cast<pid_t>(process.processId());
        QVERIFY(processGroup > 0);
        QCOMPARE(::kill(-processGroup, SIGTERM), 0);
        QVERIFY(process.waitForFinished(3000));
        QVERIFY(!hasTemporaryFiles(home.home.path(), QStringLiteral(".couchplay-timeout.*")));
    }

    void testHostStartReportsOnlyAnObservedSteamProcess()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("host operations require Linux procfs");
        }
        HostTestHome home;
        QVERIFY(home.ready);
        const QString binary = home.steamRoot + QStringLiteral("/ubuntu12_64/steam");
        QVERIFY(QDir().mkpath(QFileInfo(binary).absolutePath()));
        QVERIFY(QFile::copy(QStringLiteral("/bin/sleep"), binary));
        QVERIFY(QFile::setPermissions(binary, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                               | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                               | QFileDevice::ReadOther | QFileDevice::ExeOther));
        const QString steamScriptPath = home.steamRoot + QStringLiteral("/steam.sh");
        const QByteArray launchingScript = "#!/bin/sh\nif [ \"$1\" = \"-shutdown\" ]; then exit 0; fi\n\"$HOME/.local/share/Steam/ubuntu12_64/steam\" 30 &\necho $! > \"$HOME/steam-pid\"\nwait\n";
        QVERIFY(writeFile(steamScriptPath, launchingScript,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                              | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                              | QFileDevice::ReadOther | QFileDevice::ExeOther));

        QProcess start;
        home.configure(start, QStringLiteral("start"), {home.steamRoot});
        start.start();
        QVERIFY(start.waitForStarted(3000));
        QVERIFY(start.waitForFinished(15000));
        QCOMPARE(start.exitCode(), 0);
        QCOMPARE(start.readAllStandardOutput().trimmed(), QByteArray("started"));
        QFile pidFile(home.home.path() + QStringLiteral("/steam-pid"));
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        bool pidOk = false;
        const pid_t pid = static_cast<pid_t>(pidFile.readAll().trimmed().toLongLong(&pidOk));
        QVERIFY(pidOk && pid > 0);
        QCOMPARE(::kill(pid, SIGTERM), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(QStringLiteral("/proc/%1/exe").arg(pid)), 3000);

        QVERIFY(writeFile(steamScriptPath, "#!/bin/sh\nexit 0\n",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                              | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                              | QFileDevice::ReadOther | QFileDevice::ExeOther));
        QProcess failedStart;
        home.configure(failedStart, QStringLiteral("start"), {home.steamRoot});
        failedStart.start();
        QVERIFY(failedStart.waitForStarted(3000));
        QVERIFY(failedStart.waitForFinished(15000));
        QVERIFY(failedStart.exitCode() != 0);
        QVERIFY(failedStart.readAllStandardOutput().trimmed() != QByteArray("started"));
        QVERIFY(failedStart.readAllStandardError().contains("steam-start-timeout"));
    }

    void testGameModeSignalDuringReadinessCleansKwin()
    {
        if (QSysInfo::kernelType() != QStringLiteral("linux")) {
            QSKIP("game mode process cleanup requires Linux");
        }
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString binDir = home.path() + QStringLiteral("/bin");
        QVERIFY(QDir().mkpath(binDir));
        const QString kwinPidPath = home.path() + QStringLiteral("/kwin-pid");
        QVERIFY(writeFile(binDir + QStringLiteral("/kwin_wayland"),
                          "#!/bin/sh\necho $$ > \"$KWINT_TEST_PID\"\nexec /bin/sleep 30\n",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QVERIFY(writeFile(binDir + QStringLiteral("/dbus-send"), "#!/bin/sh\nexit 1\n",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QVERIFY(writeFile(binDir + QStringLiteral("/sleep"),
                          "#!/bin/sh\n: > \"$READINESS_SLEEP_MARKER\"\nexec /bin/sleep \"$@\"\n",
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("HOME"), home.path());
        environment.insert(QStringLiteral("PATH"), binDir + QLatin1Char(':') + environment.value(QStringLiteral("PATH")));
        environment.insert(QStringLiteral("GAMESCOPE_WAYLAND_DISPLAY"), QStringLiteral("gamescope-0"));
        environment.insert(QStringLiteral("KWINT_TEST_PID"), kwinPidPath);
        environment.insert(QStringLiteral("READINESS_SLEEP_MARKER"), home.path() + QStringLiteral("/sleep-marker"));
        process.setProcessEnvironment(environment);
        process.setProgram(QStringLiteral("/bin/bash"));
        process.setArguments({QStringLiteral("-c"), gameModeScript(), QStringLiteral("gamemode-test"),
                              QStringLiteral("--couchplay-native"), QStringLiteral("/bin/true"), QStringLiteral("--")});
        process.start();
        QVERIFY(process.waitForStarted(3000));
        const QString sleepMarker = home.path() + QStringLiteral("/sleep-marker");
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(kwinPidPath) && QFile::exists(sleepMarker), 3000);
        QFile pidFile(kwinPidPath);
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        bool ok = false;
        const pid_t kwinPid = static_cast<pid_t>(pidFile.readAll().trimmed().toLongLong(&ok));
        QVERIFY(ok && kwinPid > 0);
        QCOMPARE(::kill(static_cast<pid_t>(process.processId()), SIGTERM), 0);
        QVERIFY(process.waitForFinished(2000));
        QCOMPARE(process.exitCode(), 143);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(QStringLiteral("/proc/%1/exe").arg(kwinPid)), 3000);
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
