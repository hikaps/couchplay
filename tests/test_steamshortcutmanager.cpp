// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QByteArray>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
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
