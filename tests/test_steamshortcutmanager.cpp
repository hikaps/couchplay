// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QSignalSpy>
#include <QTest>

#include "SteamShortcutManager.h"

class TestSteamShortcutManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
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
