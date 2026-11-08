// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "VirtualDisplayManager.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
QString writeScript(const QTemporaryDir &dir, const QString &name, const QString &body)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    file.write("#!/bin/sh\n" + body.toUtf8() + "\n");
    file.close();
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

VirtualDisplayManager::ScreenLookup fixedLookup(const QString &name, const QRect &geometry)
{
    return [name, geometry](const QString &, const QSize &, const QSet<QString> &, QString *outName, QRect *outGeometry) {
        *outName = name;
        *outGeometry = geometry;
        return true;
    };
}
}

class TestVirtualDisplayManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void readyReportsOutputAndGeometry()
    {
        QTemporaryDir dir;
        VirtualDisplayManager manager;
        manager.setProgramOverride(writeScript(dir, QStringLiteral("vd"), QStringLiteral("echo 'READY 42'\nexec sleep 30")));
        manager.setScreenLookup(fixedLookup(QStringLiteral("Virtual-couchplay-p2"), QRect(3840, 0, 1280, 720)));
        QSignalSpy ready(&manager, &VirtualDisplayManager::ready);
        QSignalSpy failed(&manager, &VirtualDisplayManager::failed);

        manager.create(1, QSize(1280, 720));

        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(ready.at(0).at(0).toInt(), 1);
        QCOMPARE(ready.at(0).at(1).toString(), QStringLiteral("Virtual-couchplay-p2"));
        QCOMPARE(ready.at(0).at(2).toRect(), QRect(3840, 0, 1280, 720));
        QCOMPARE(failed.count(), 0);
        QVERIFY(manager.hasDisplays());
        QCOMPARE(manager.outputName(1), QStringLiteral("Virtual-couchplay-p2"));

        manager.destroyAll();
        QVERIFY(!manager.hasDisplays());
    }

    void passesNameAndSizeArguments()
    {
        QTemporaryDir dir;
        const QString argsFile = dir.filePath(QStringLiteral("args"));
        VirtualDisplayManager manager;
        manager.setProgramOverride(writeScript(dir,
                                               QStringLiteral("vd"),
                                               QStringLiteral("echo \"$@\" > '%1'\necho 'READY 1'\nexec sleep 30").arg(argsFile)));
        manager.setScreenLookup(fixedLookup(QStringLiteral("Virtual-couchplay-p1"), QRect(0, 0, 1920, 1080)));
        QSignalSpy ready(&manager, &VirtualDisplayManager::ready);

        manager.create(0, QSize(1920, 1080));

        QTRY_COMPARE(ready.count(), 1);
        QFile file(argsFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()).trimmed(),
                 QStringLiteral("--name couchplay-p1 --width 1920 --height 1080"));
    }

    void earlyExitFailsWithStderr()
    {
        QTemporaryDir dir;
        VirtualDisplayManager manager;
        manager.setProgramOverride(writeScript(dir, QStringLiteral("vd"), QStringLiteral("echo 'no protocol' >&2\nexit 2")));
        QSignalSpy ready(&manager, &VirtualDisplayManager::ready);
        QSignalSpy failed(&manager, &VirtualDisplayManager::failed);

        manager.create(0, QSize(1280, 720));

        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toInt(), 0);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("no protocol")));
        QCOMPARE(ready.count(), 0);
        QVERIFY(!manager.hasDisplays());
    }

    void missingScreenTimesOut()
    {
        QTemporaryDir dir;
        VirtualDisplayManager manager;
        manager.setProgramOverride(writeScript(dir, QStringLiteral("vd"), QStringLiteral("echo 'READY 1'\nexec sleep 30")));
        manager.setScreenLookup([](const QString &, const QSize &, const QSet<QString> &, QString *, QRect *) {
            return false;
        });
        manager.setTimeoutMs(500);
        QSignalSpy ready(&manager, &VirtualDisplayManager::ready);
        QSignalSpy failed(&manager, &VirtualDisplayManager::failed);

        manager.create(0, QSize(1280, 720));

        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("Timed out")));
        QCOMPARE(ready.count(), 0);
        QVERIFY(!manager.hasDisplays());
    }

    void missingBinaryFails()
    {
        VirtualDisplayManager manager;
        manager.setProgramOverride(QStringLiteral("/nonexistent/couchplay-vdisplay"));
        QSignalSpy failed(&manager, &VirtualDisplayManager::failed);

        manager.create(0, QSize(1280, 720));

        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(!manager.hasDisplays());
    }

    void destroyAllTerminatesProcesses()
    {
        QTemporaryDir dir;
        const QString pidFile = dir.filePath(QStringLiteral("pid"));
        VirtualDisplayManager manager;
        manager.setProgramOverride(writeScript(dir,
                                               QStringLiteral("vd"),
                                               QStringLiteral("echo $$ > '%1'\necho 'READY 1'\nexec sleep 30").arg(pidFile)));
        manager.setScreenLookup(fixedLookup(QStringLiteral("Virtual-couchplay-p1"), QRect(0, 0, 1280, 720)));
        QSignalSpy ready(&manager, &VirtualDisplayManager::ready);
        manager.create(0, QSize(1280, 720));
        QTRY_COMPARE(ready.count(), 1);

        QFile file(pidFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const qint64 pid = file.readAll().trimmed().toLongLong();
        QVERIFY(pid > 0);

        manager.destroyAll();

        // The shell exec'd into sleep with the same pid; it must be gone.
        QTRY_VERIFY(QFile::exists(QStringLiteral("/proc/%1").arg(pid)) == false);
    }

    void expectedOutputNameIsOneBased()
    {
        QCOMPARE(VirtualDisplayManager::expectedOutputName(0), QStringLiteral("Virtual-couchplay-p1"));
        QCOMPARE(VirtualDisplayManager::expectedOutputName(3), QStringLiteral("Virtual-couchplay-p4"));
    }
};

QTEST_MAIN(TestVirtualDisplayManager)
#include "test_virtualdisplaymanager.moc"
