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

    void testSkipPairingCheck()
    {
        QString error;
        CommandLineRequest request = CommandLineOptions::parse(
            {QStringLiteral("couchplay"), QStringLiteral("--profile"), QStringLiteral("P"), QStringLiteral("--start"),
             QStringLiteral("--skip-pairing-check")},
            &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(request.skipPairingCheck);

        request = CommandLineOptions::parse(
            {QStringLiteral("couchplay"), QStringLiteral("--profile"), QStringLiteral("P"), QStringLiteral("--start")},
            &error);
        QVERIFY(!request.skipPairingCheck);

        error.clear();
        QVERIFY(!CommandLineOptions::parse({QStringLiteral("couchplay"), QStringLiteral("--skip-pairing-check")}, &error)
                     .requested());
        QVERIFY(!error.isEmpty());

        error.clear();
        QVERIFY(!CommandLineOptions::parse({QStringLiteral("couchplay"), QStringLiteral("--profile"), QStringLiteral("P"),
                                            QStringLiteral("--start"), QStringLiteral("--skip-pairing-check"),
                                            QStringLiteral("--skip-pairing-check")},
                                           &error)
                     .requested());
        QVERIFY(!error.isEmpty());
    }

    void testGame()
    {
        const auto parse = [](const QStringList &extra, QString *error) {
            QStringList arguments = {QStringLiteral("couchplay"), QStringLiteral("--profile"), QStringLiteral("P"),
                                     QStringLiteral("--start")};
            arguments += extra;
            return CommandLineOptions::parse(arguments, error);
        };
        QString error;
        CommandLineRequest request = parse({QStringLiteral("--game"), QStringLiteral("steam:49520")}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request.game.launcherId, QStringLiteral("steam"));
        QCOMPARE(request.game.backend, QStringLiteral("native"));
        QCOMPARE(request.game.gameId, QStringLiteral("49520"));

        request = parse({QStringLiteral("--game"), QStringLiteral("heroic:gog:Some:App")}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request.game.launcherId, QStringLiteral("heroic"));
        QCOMPARE(request.game.backend, QStringLiteral("gog"));
        QCOMPARE(request.game.gameId, QStringLiteral("Some:App"));

        const QList<QStringList> invalid = {
            {QStringLiteral("--game"), QStringLiteral("steam:abc")},
            {QStringLiteral("--game"), QStringLiteral("steam:0")},
            {QStringLiteral("--game"), QStringLiteral("steam:")},
            {QStringLiteral("--game"), QStringLiteral("heroic:epic:x")},
            {QStringLiteral("--game"), QStringLiteral("heroic:gog:")},
            {QStringLiteral("--game"), QStringLiteral("epic:1")},
            {QStringLiteral("--game")},
            {QStringLiteral("--game"), QStringLiteral("steam:1"), QStringLiteral("--game"), QStringLiteral("steam:2")},
        };
        for (const QStringList &extra : invalid) {
            error.clear();
            QVERIFY2(!parse(extra, &error).requested(), qPrintable(extra.join(QLatin1Char(' '))));
            QVERIFY2(!error.isEmpty(), qPrintable(extra.join(QLatin1Char(' '))));
        }

        error.clear();
        QVERIFY(!CommandLineOptions::parse({QStringLiteral("couchplay"), QStringLiteral("--profile"), QStringLiteral("P"),
                                            QStringLiteral("--game"), QStringLiteral("steam:1")},
                                           &error)
                     .requested());
        QVERIFY(!error.isEmpty());
    }

    void testQuick()
    {
        const auto parse = [](const QStringList &args, QString *error) {
            return CommandLineOptions::parse(QStringList{QStringLiteral("couchplay")} + args, error);
        };
        QString error;
        CommandLineRequest request = parse({QStringLiteral("--quick"), QStringLiteral("3"), QStringLiteral("--game"),
                                            QStringLiteral("steam:440")},
                                           &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request.quickPlayers, 3);
        QVERIFY(request.start);
        QVERIFY(request.requested());
        QCOMPARE(request.game.gameId, QStringLiteral("440"));

        request = parse({QStringLiteral("--quick"), QStringLiteral("2"), QStringLiteral("--game"), QStringLiteral("steam:440"),
                         QStringLiteral("--start"), QStringLiteral("--exit-after-session")},
                        &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(request.exitAfterSession);

        const QList<QStringList> invalid = {
            {QStringLiteral("--quick"), QStringLiteral("1"), QStringLiteral("--game"), QStringLiteral("steam:1")},
            {QStringLiteral("--quick"), QStringLiteral("5"), QStringLiteral("--game"), QStringLiteral("steam:1")},
            {QStringLiteral("--quick"), QStringLiteral("x"), QStringLiteral("--game"), QStringLiteral("steam:1")},
            {QStringLiteral("--quick")},
            {QStringLiteral("--quick"), QStringLiteral("2")},
            {QStringLiteral("--quick"), QStringLiteral("2"), QStringLiteral("--quick"), QStringLiteral("3"),
             QStringLiteral("--game"), QStringLiteral("steam:1")},
            {QStringLiteral("--profile"), QStringLiteral("P"), QStringLiteral("--quick"), QStringLiteral("2"),
             QStringLiteral("--game"), QStringLiteral("steam:1")},
            {QStringLiteral("--start")},
        };
        for (const QStringList &args : invalid) {
            error.clear();
            QVERIFY2(!parse(args, &error).requested(), qPrintable(args.join(QLatin1Char(' '))));
            QVERIFY2(!error.isEmpty(), qPrintable(args.join(QLatin1Char(' '))));
        }
    }

    void testLauncher()
    {
        const auto parse = [](const QStringList &args, QString *error) {
            return CommandLineOptions::parse(QStringList{QStringLiteral("couchplay")} + args, error);
        };
        QString error;
        CommandLineRequest request = parse({QStringLiteral("--quick"), QStringLiteral("2"), QStringLiteral("--launcher"),
                                            QStringLiteral("Lutris")},
                                           &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request.launcher, QStringLiteral("Lutris"));
        QVERIFY(request.start);
        QVERIFY(request.requested());

        request = parse({QStringLiteral("--profile"), QStringLiteral("P"), QStringLiteral("--start"),
                         QStringLiteral("--launcher"), QStringLiteral("custom-1")},
                        &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request.launcher, QStringLiteral("custom-1"));

        request = parse({QStringLiteral("--quick"), QStringLiteral("2"), QStringLiteral("--launcher"),
                         QStringLiteral("Lutris"), QStringLiteral("--game"), QStringLiteral("steam:440")},
                        &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        const QList<QStringList> invalid = {
            {QStringLiteral("--launcher"), QStringLiteral("X")},
            {QStringLiteral("--profile"), QStringLiteral("P"), QStringLiteral("--launcher"), QStringLiteral("X")},
            {QStringLiteral("--quick"), QStringLiteral("2"), QStringLiteral("--launcher")},
            {QStringLiteral("--quick"), QStringLiteral("2"), QStringLiteral("--launcher"), QStringLiteral("A"),
             QStringLiteral("--launcher"), QStringLiteral("B")},
        };
        for (const QStringList &args : invalid) {
            error.clear();
            QVERIFY2(!parse(args, &error).requested(), qPrintable(args.join(QLatin1Char(' '))));
            QVERIFY2(!error.isEmpty(), qPrintable(args.join(QLatin1Char(' '))));
        }
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

    void testShortProfileOption()
    {
        QString error;
        const CommandLineRequest request = CommandLineOptions::parse(
            {QStringLiteral("couchplay"), QStringLiteral("-p"), QStringLiteral("Short")}, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(request.profileName, QStringLiteral("Short"));
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
