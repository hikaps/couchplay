// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QSignalSpy>
#include <QTest>

#include <csignal>

#include "SignalBridge.h"

namespace {
struct sigaction currentAction(int signalNumber)
{
    struct sigaction current {};
    ::sigaction(signalNumber, nullptr, &current);
    return current;
}

bool sameDisposition(const struct sigaction &a, const struct sigaction &b)
{
    return a.sa_handler == b.sa_handler && a.sa_flags == b.sa_flags;
}
}

class TestSignalBridge : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testTerminationSignalsReachTheEventLoop()
    {
        SignalBridge bridge;
        QVERIFY(bridge.isActive());
        QSignalSpy spy(&bridge, &SignalBridge::terminationRequested);

        for (const int signalNumber : {SIGTERM, SIGINT, SIGHUP}) {
            spy.clear();
            QCOMPARE(::raise(signalNumber), 0);
            QTRY_COMPARE(spy.count(), 1);
            QCOMPARE(spy.constFirst().constFirst().toInt(), signalNumber);
        }
    }

    void testPreviousDispositionIsRestoredOnDestruction()
    {
        // The parent (test runner, dbus-run-session) may have customised these, so compare to the original.
        const int handled[] = {SIGTERM, SIGINT, SIGHUP};
        struct sigaction before[3];
        for (int i = 0; i < 3; ++i) {
            before[i] = currentAction(handled[i]);
        }
        {
            SignalBridge bridge;
            QVERIFY(bridge.isActive());
            for (int i = 0; i < 3; ++i) {
                QVERIFY(!sameDisposition(currentAction(handled[i]), before[i]));
            }
        }
        for (int i = 0; i < 3; ++i) {
            QVERIFY(sameDisposition(currentAction(handled[i]), before[i]));
        }
    }

    void testOnlyOneInstanceOwnsTheSignals()
    {
        SignalBridge first;
        QVERIFY(first.isActive());
        {
            SignalBridge second;
            QVERIFY(!second.isActive());
        }
        // Destroying the inactive instance must not uninstall the active one.
        QSignalSpy spy(&first, &SignalBridge::terminationRequested);
        QCOMPARE(::raise(SIGTERM), 0);
        QTRY_COMPARE(spy.count(), 1);
    }
};

QTEST_MAIN(TestSignalBridge)
#include "test_signalbridge.moc"
