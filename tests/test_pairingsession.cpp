// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include <QDir>
#include <QFile>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#define private public
#include "SessionRunner.h"
#undef private
#include "PairingSession.h"
#include "../src/core/SunshineConfig.h"

namespace
{
const QString kUser = QStringLiteral("pairingsession-test");

QString writeScript(const QTemporaryDir &dir, const QString &name, const QByteArray &body)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    file.write("#!/bin/sh\n" + body + "\n");
    file.close();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

// Sunshine stand-in: reports the web UI line (ready) or the capture failure (fatal), then stays alive
QByteArray fakeSunshine(bool works)
{
    QByteArray script =
        "conf=\"$1\"\ndir=$(dirname \"$conf\")\n"
        "if [ \"$2\" = \"--creds\" ]; then echo '{}' > \"$dir/credentials.json\"; exit 0; fi\n"
        "log=$(sed -n 's/^log_path = //p' \"$conf\")\n"
        "echo run >> \"$dir/runs\"\n"
        "scheme=https:/\n";
    if (works) {
        script += "echo \"Info: Configuration UI available at [${scheme}/localhost:48020]\" >> \"$log\"\n";
    } else {
        script += "echo \"Error: Unable to find display or encoder during startup.\" >> \"$log\"\n";
    }
    script += "exec sleep 30";
    return script;
}

VirtualDisplayManager::ScreenLookup fixedLookup()
{
    return [](const QString &name, const QSize &size, const QSet<QString> &, QString *outName, QRect *geometry) {
        *outName = name;
        *geometry = QRect(QPoint(5000, 0), size);
        return true;
    };
}

QString runsFile()
{
    return SunshineConfig::stateDirectory(kUser) + QStringLiteral("/runs");
}
}

class TestPairingSession : public QObject
{
    Q_OBJECT

private:
    void configure(PairingSession &session, const QTemporaryDir &dir, bool vdisplayWorks, bool sunshineWorks)
    {
        session.setVirtualDisplayProgramOverride(
            writeScript(dir,
                        QStringLiteral("vdisplay"),
                        vdisplayWorks ? QByteArray("echo 'READY 1'\nexec sleep 30")
                                      : QByteArray("echo 'protocol missing' >&2\nexit 2")));
        session.setScreenLookup(fixedLookup());
        session.setSunshineProgramOverride(writeScript(dir, QStringLiteral("sunshine"), fakeSunshine(sunshineWorks)));
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        QDir(SunshineConfig::stateDirectory(kUser)).removeRecursively();
    }

    void cleanup()
    {
        QDir(SunshineConfig::stateDirectory(kUser)).removeRecursively();
    }

    void emptyUsernameRefused()
    {
        PairingSession session;
        session.start(QString());
        QCOMPARE(session.state(), PairingSession::Failed);
        QVERIFY(session.error().contains(QStringLiteral("gaming user")));
        QVERIFY(!session.busy());
        QVERIFY(!session.virtualDisplays()->hasDisplays());
    }

    void activeSessionRefused()
    {
        SessionRunner runner;
        runner.setActive(true);
        PairingSession session;
        session.setSessionRunner(&runner);
        session.start(kUser);
        QCOMPARE(session.state(), PairingSession::Failed);
        QVERIFY(!session.error().isEmpty());
        QVERIFY(!session.virtualDisplays()->hasDisplays());
    }

    void sessionBecomingActiveStopsPairing()
    {
        QTemporaryDir dir;
        SessionRunner runner;
        PairingSession session;
        session.setSessionRunner(&runner);
        configure(session, dir, true, true);

        session.start(kUser);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), PairingSession::Ready, 10000);

        runner.setActive(true);
        QCOMPARE(session.state(), PairingSession::Idle);
        QVERIFY(!session.virtualDisplays()->hasDisplays());
        QTRY_VERIFY(!session.busy());
    }

    void happyPathReachesReadyThenStopTearsDown()
    {
        QTemporaryDir dir;
        PairingSession session;
        configure(session, dir, true, true);
        QSignalSpy paired(&session, &PairingSession::pairedChanged);

        session.start(kUser);
        QCOMPARE(session.state(), PairingSession::Starting);
        QVERIFY(session.busy());
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), PairingSession::Ready, 10000);
        QCOMPARE(session.username(), kUser);
        QVERIFY(session.virtualDisplays()->hasDisplays());
        QVERIFY(session.streamManager()->hasOwnedPids());

        session.stop();
        QCOMPARE(session.state(), PairingSession::Idle);
        QVERIFY(!session.virtualDisplays()->hasDisplays());
        QTRY_VERIFY(!session.streamManager()->hasOwnedPids());
        QTRY_VERIFY(!session.busy());
        QCOMPARE(paired.count(), 1);
        QCOMPARE(paired.at(0).at(0).toString(), kUser);
    }

    void sunshineBindsTheStableUserPort()
    {
        QTemporaryDir dir;
        PairingSession session;
        configure(session, dir, true, true);

        session.start(kUser);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), PairingSession::Ready, 10000);

        const QVariantList streams = session.streamManager()->streams();
        QCOMPARE(streams.size(), 1);
        const int stored = SunshineConfig::userPort(kUser);
        QVERIFY(stored > 0);
        QCOMPARE(streams.at(0).toMap().value(QStringLiteral("port")).toInt(), stored);
        QCOMPARE(session.hostPort(kUser), QString::number(stored));
    }

    void unpairWithoutClientIsRefused()
    {
        PairingSession session;
        QSignalSpy finished(&session, &PairingSession::unpairFinished);

        session.unpairClient(kUser, QString());
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.at(0).at(1).toBool());
        QVERIFY(!session.busy());
    }

    void unpairDuringSessionNeedsTheUsersStream()
    {
        SessionRunner runner;
        runner.setActive(true);
        PairingSession session;
        session.setSessionRunner(&runner);
        QSignalSpy finished(&session, &PairingSession::unpairFinished);

        session.unpairClient(kUser, QStringLiteral("some-uuid"));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.at(0).at(0).toString(), kUser);
        QVERIFY(!finished.at(0).at(1).toBool());
        QVERIFY(!session.virtualDisplays()->hasDisplays());
    }

    void displayFailureFailsWithoutSunshine()
    {
        QTemporaryDir dir;
        PairingSession session;
        configure(session, dir, false, true);

        session.start(kUser);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), PairingSession::Failed, 10000);
        QVERIFY(session.error().contains(QStringLiteral("virtual display")));
        QVERIFY(!session.streamManager()->hasOwnedPids());
        QVERIFY(!QFile::exists(runsFile()));
        QVERIFY(!session.virtualDisplays()->hasDisplays());
    }

    void sunshineFailureFailsAndDestroysDisplay()
    {
        QTemporaryDir dir;
        PairingSession session;
        configure(session, dir, true, false);

        session.start(kUser);
        QTRY_COMPARE_WITH_TIMEOUT(session.state(), PairingSession::Failed, 10000);
        QVERIFY(!session.error().isEmpty());
        QVERIFY(!session.virtualDisplays()->hasDisplays());
        QTRY_VERIFY(!session.streamManager()->hasOwnedPids());
        QTRY_VERIFY(!session.busy());
    }
};

QTEST_MAIN(TestPairingSession)
#include "test_pairingsession.moc"
