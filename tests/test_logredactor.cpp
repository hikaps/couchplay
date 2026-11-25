// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include <QTest>

#include "Logging.h"

class TestLogRedactor : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // The desktop user is read once, on the first redact() call
        qputenv("USER", "desktopuser");
        LogRedactor::setPlayerNames({QStringLiteral("alice"), QStringLiteral("couch-2"), QStringLiteral("ab")});
    }

    void testMasksPersonalData_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");

        const auto row = [](const char *name, const char *input, const char *expected) {
            QTest::newRow(name) << QString::fromUtf8(input) << QString::fromUtf8(expected);
        };
        row("steamid64", "owner 76561198012345678 logged in", "owner <steamid> logged in");
        row("userdata dir", "/home/x/.steam/userdata/91259273/config", "/home/x/.steam/userdata/<steamid>/config");
        row("account id line", "Target Steam ID: 91259273", "Target Steam ID: <steamid>");
        row("stable id quoted", "type gamepad stableId \"e4:17:d8:00:11:22\"", "type gamepad stableId <device>");
        row("stable id serial", "(stableId: ABC123XYZ)", "(stableId: <device>)");
        row("mac", "paired aa:bb:cc:dd:ee:ff now", "paired <mac> now");
        row("public ipv4", "peer 203.0.113.7:47989", "peer <ip>:47989");
        row("ipv6", "peer 2001:db8:0:1:2:3:4:5 up", "peer <ip> up");
        row("desktop user in path", "/var/home/desktopuser/.local/share", "/var/home/<user>/.local/share");
        row("player name", "Syncing Steam shortcuts for player alice", "Syncing Steam shortcuts for player player-2bd8");
        row("player name with dash in path", "/players/couch-2/home", "/players/player-7113/home");
    }

    void testMasksPersonalData()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(LogRedactor::redact(input), expected);
    }

    void testKeepsHarmlessText()
    {
        // Loopback, version strings, a name that only contains a player name, and a too-short player name
        const QString text = QStringLiteral("127.0.0.1:36479 0.0.0.0 version 6.10.28.19225.0 alicebob couch-25 ab");
        QCOMPARE(LogRedactor::redact(text), text);
    }

    void testSamePlayerGetsSameTag()
    {
        const QString once = LogRedactor::redact(QStringLiteral("alice"));
        QCOMPARE(LogRedactor::redact(QStringLiteral("x alice y alice")), QStringLiteral("x %1 y %1").arg(once));
        QVERIFY(once.startsWith(QStringLiteral("player-")));
        QVERIFY(once != LogRedactor::redact(QStringLiteral("couch-2")));
    }

    void testPlayerNamesFollowTheList()
    {
        LogRedactor::setPlayerNames({});
        QCOMPARE(LogRedactor::redact(QStringLiteral("alice")), QStringLiteral("alice"));
    }
};

QTEST_MAIN(TestLogRedactor)
#include "test_logredactor.moc"
