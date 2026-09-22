// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QApplication>
#include <QTest>

#include "SessionLaunchClient.h"

class TestSessionLaunchClient : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRejectsNonWaitingRequest()
    {
        auto *application = qobject_cast<QApplication *>(QCoreApplication::instance());
        QVERIFY(application);
        CommandLineRequest request;
        request.profileName = QStringLiteral("Profile");
        request.start = true;
        QVERIFY(!request.exitAfterSession);
        QCOMPARE(SessionLaunchClient::run(*application, request, QStringLiteral("com.github.CouchPlay")), 2);
    }
};

QTEST_MAIN(TestSessionLaunchClient)
#include "test_sessionlaunchclient.moc"
