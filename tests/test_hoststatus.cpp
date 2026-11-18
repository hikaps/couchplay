// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include <QProcess>
#include <QTest>

#include "../src/core/HostStatus.h"

class TestHostStatus : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void emptyOutputReportsNothing()
    {
        const HostStatus::Result result = HostStatus::parse(QString());
        QVERIFY(result.missingTools.isEmpty());
        QVERIFY(result.uinputWritable);
        QVERIFY(!result.legacyHelperFound);
    }

    void parsesMissingToolsWithPackageNames()
    {
        const HostStatus::Result result = HostStatus::parse(QStringLiteral("missing bwrap\nmissing dbus-run-session\n"));
        QCOMPARE(result.missingTools,
                 (QStringList{QStringLiteral("bwrap (package: bubblewrap)"),
                              QStringLiteral("dbus-run-session (package: dbus)")}));
    }

    void ignoresUnknownLines()
    {
        const HostStatus::Result result = HostStatus::parse(QStringLiteral("missing rm -rf\nrandom\n\n"));
        QVERIFY(result.missingTools.isEmpty());
        QVERIFY(result.uinputWritable);
        QVERIFY(!result.legacyHelperFound);
    }

    void parsesUinputAndLegacyHelper()
    {
        const HostStatus::Result result = HostStatus::parse(QStringLiteral("nouinput\nlegacy\n"));
        QVERIFY(!result.uinputWritable);
        QVERIFY(result.legacyHelperFound);
    }

    // The real probe script, run with the host's shell: every output line must be one the parser understands.
    void probeScriptOutputIsParseable()
    {
        QProcess sh;
        sh.start(QStringLiteral("sh"), {QStringLiteral("-c"), HostStatus::probeScript()});
        QVERIFY(sh.waitForFinished(10000));
        QCOMPARE(sh.exitCode(), 0);
        const QStringList lines = QString::fromLocal8Bit(sh.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            QVERIFY2(line.startsWith(QLatin1String("missing ")) || line == QLatin1String("nouinput")
                         || line == QLatin1String("legacy"),
                     qPrintable(line));
        }
    }

    void migrationCommandNamesTheUser()
    {
        qputenv("USER", "alice");
        HostStatus status;
        QVERIFY(status.migrationCommand().startsWith(QStringLiteral("sudo bash migrate-from-helper.sh --user alice")));
    }
};

QTEST_MAIN(TestHostStatus)
#include "test_hoststatus.moc"
