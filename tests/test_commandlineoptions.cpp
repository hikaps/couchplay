// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QTest>

#include "CommandLineOptions.h"

class TestCommandLineOptions : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testEmptyRequest()
    {
        QString error;
        const CommandLineRequest request = CommandLineOptions::parse({QStringLiteral("couchplay")}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!request.requested());
        QVERIFY(!request.start);
        QVERIFY(!request.exitAfterSession);
    }

    void testProfileAndStart()
    {
        QString error;
        const CommandLineRequest request = CommandLineOptions::parse(
            {QStringLiteral("couchplay"), QStringLiteral("--profile"), QStringLiteral("Family Night"),
             QStringLiteral("--start"), QStringLiteral("--exit-after-session")},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request.profileName, QStringLiteral("Family Night"));
        QVERIFY(request.start);
        QVERIFY(request.exitAfterSession);
    }

    void testForwardedArgumentList()
    {
        QString error;
        const CommandLineRequest request = CommandLineOptions::parse(
            {QStringLiteral("--profile"), QStringLiteral("Forwarded"), QStringLiteral("--start")}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request.profileName, QStringLiteral("Forwarded"));
        QVERIFY(request.start);
    }

    void testInvalidCombinations()
    {
        QString error;
        QVERIFY(!CommandLineOptions::parse({QStringLiteral("couchplay"), QStringLiteral("--start")}, &error).requested());
        QVERIFY(!error.isEmpty());

        error.clear();
        QVERIFY(!CommandLineOptions::parse({QStringLiteral("couchplay"), QStringLiteral("--exit-after-session")}, &error)
                     .requested());
        QVERIFY(!error.isEmpty());

        error.clear();
        QVERIFY(!CommandLineOptions::parse({QStringLiteral("couchplay"), QStringLiteral("--profile")}, &error).requested());
        QVERIFY(!error.isEmpty());
    }

    void testUnknownArgument()
    {
        QString error;
        QVERIFY(!CommandLineOptions::parse(
                         {QStringLiteral("couchplay"), QStringLiteral("--unknown")}, &error)
                     .requested());
        QCOMPARE(error, QStringLiteral("Unknown command-line argument: --unknown"));
    }
};

QTEST_MAIN(TestCommandLineOptions)
#include "test_commandlineoptions.moc"
