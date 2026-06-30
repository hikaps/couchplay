// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QStandardPaths>
#include <QTimer>

#define private public
#include "StreamManager.h"
#undef private

#include "SunshineConfig.h"

class TestStreamManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // State tracking tests
    void testInitialState();
    void testStreamStateUnknownIndex();
    void testIsStreamingUnknownIndex();

    // Property tests
    void testAutoRestartDefault();
    void testSetAutoRestart();
    void testAutoRestartSignal();
    void testAutoRestartNoSignal();

    // streams() property tests
    void testStreamsEmptyInitially();
    void testStreamsFormatWithEntry();
    void testStreamsMultipleEntries();

    // startStream validation tests
    void testStartStreamNegativeIndex();
    void testStartStreamEmptyUsername();
    void testStartStreamAlreadyActive();

    // stopStream tests
    void testStopStreamNonExistent();
    void testStopStreamWithZeroPid();
    void testStopAllEmpty();
    void testStopAllWithMultipleEntries();

    // Helper unavailable path (assumes no CouchPlayHelper D-Bus service)
    void testStartStreamHelperUnavailable();
    // Startup timeout transition (Waiting → Streaming)
    void testStartupTimeoutTransition();

    // Auto-restart state machine (onHelperInstanceStopped / port-bump / restart)
    void testCrashDuringStartupBumpsPortAndSchedulesRestart();
    void testCrashAfterStreamingKeepsPort();
    void testAutoRestartDisabledRemovesImmediately();
    void testStopStreamCancelsPendingRestart();

private:
    StreamManager *m_manager = nullptr;
    QTemporaryDir *m_tempDir = nullptr;
};

void TestStreamManager::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
}

void TestStreamManager::cleanupTestCase()
{
    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestStreamManager::init()
{
    m_manager = new StreamManager(this);
}

void TestStreamManager::cleanup()
{
    delete m_manager;
    m_manager = nullptr;
}

// --- State tracking tests ---

void TestStreamManager::testInitialState()
{
    QVERIFY(m_manager->streams().isEmpty());
    QCOMPARE(m_manager->streamState(0), StreamManager::NotStarted);
    QCOMPARE(m_manager->streamState(1), StreamManager::NotStarted);
    QVERIFY(!m_manager->isStreaming(0));
    QVERIFY(!m_manager->isStreaming(1));
}

void TestStreamManager::testStreamStateUnknownIndex()
{
    QCOMPARE(m_manager->streamState(-1), StreamManager::NotStarted);
    QCOMPARE(m_manager->streamState(100), StreamManager::NotStarted);
}

void TestStreamManager::testIsStreamingUnknownIndex()
{
    QVERIFY(!m_manager->isStreaming(-1));
    QVERIFY(!m_manager->isStreaming(0));
    QVERIFY(!m_manager->isStreaming(999));
}

// --- Property tests ---

void TestStreamManager::testAutoRestartDefault()
{
    QVERIFY(m_manager->autoRestart());
}

void TestStreamManager::testSetAutoRestart()
{
    m_manager->setAutoRestart(false);
    QVERIFY(!m_manager->autoRestart());

    m_manager->setAutoRestart(true);
    QVERIFY(m_manager->autoRestart());
}

void TestStreamManager::testAutoRestartSignal()
{
    QSignalSpy spy(m_manager, &StreamManager::autoRestartChanged);

    m_manager->setAutoRestart(false);
    QCOMPARE(spy.count(), 1);

    m_manager->setAutoRestart(true);
    QCOMPARE(spy.count(), 2);
}

void TestStreamManager::testAutoRestartNoSignal()
{
    QSignalSpy spy(m_manager, &StreamManager::autoRestartChanged);

    m_manager->setAutoRestart(true); // Same as default
    QCOMPARE(spy.count(), 0);

    m_manager->setAutoRestart(false);
    m_manager->setAutoRestart(false); // Same as current
    QCOMPARE(spy.count(), 1);
}

// --- streams() property tests ---

void TestStreamManager::testStreamsEmptyInitially()
{
    QVERIFY(m_manager->streams().isEmpty());
}

void TestStreamManager::testStreamsFormatWithEntry()
{
    StreamManager::StreamEntry entry;
    entry.state = StreamManager::Streaming;
    entry.pid = 12345;
    entry.configDir = QStringLiteral("/tmp/test-config-0");
    entry.instanceIndex = 0;
    m_manager->m_streams[0] = entry;

    QVariantList streams = m_manager->streams();
    QCOMPARE(streams.size(), 1);

    QVariantMap map = streams.at(0).toMap();
    QCOMPARE(map.value(QStringLiteral("instanceIndex")).toInt(), 0);
    QCOMPARE(map.value(QStringLiteral("state")).toInt(), static_cast<int>(StreamManager::Streaming));
    QCOMPARE(map.value(QStringLiteral("pid")).toLongLong(), qint64(12345));
    QCOMPARE(map.value(QStringLiteral("configDir")).toString(), QStringLiteral("/tmp/test-config-0"));
}

void TestStreamManager::testStreamsMultipleEntries()
{
    for (int i = 0; i < 3; ++i) {
        StreamManager::StreamEntry entry;
        entry.state = StreamManager::Streaming;
        entry.pid = 1000 + i;
        entry.configDir = QStringLiteral("/tmp/test-config-%1").arg(i);
        entry.instanceIndex = i;
        m_manager->m_streams[i] = entry;
    }

    QVariantList streams = m_manager->streams();
    QCOMPARE(streams.size(), 3);

    for (const QVariant &item : streams) {
        QVariantMap map = item.toMap();
        QVERIFY(map.contains(QStringLiteral("instanceIndex")));
        QVERIFY(map.contains(QStringLiteral("state")));
        QVERIFY(map.contains(QStringLiteral("pid")));
        QVERIFY(map.contains(QStringLiteral("configDir")));
    }
}

// --- startStream validation tests ---

void TestStreamManager::testStartStreamNegativeIndex()
{
    QVariantMap config;
    config.insert(QStringLiteral("username"), QStringLiteral("player1"));

    QVERIFY(!m_manager->startStream(-1, config));
    QVERIFY(m_manager->streams().isEmpty());
}

void TestStreamManager::testStartStreamEmptyUsername()
{
    QSignalSpy errorSpy(m_manager, &StreamManager::streamError);

    QVariantMap config;
    // No username — should fail validation before D-Bus

    QVERIFY(!m_manager->startStream(0, config));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toInt(), 0);
    QVERIFY(m_manager->streams().isEmpty());
}

void TestStreamManager::testStartStreamAlreadyActive()
{
    // Test rejection when entry exists in any state
    const QList<StreamManager::StreamState> statesToTest = {
        StreamManager::Streaming,
        StreamManager::Waiting,
        StreamManager::Error
    };

    for (StreamManager::StreamState state : statesToTest) {
        StreamManager::StreamEntry entry;
        entry.state = state;
        entry.pid = 12345;
        entry.instanceIndex = 0;
        m_manager->m_streams[0] = entry;

        QSignalSpy errorSpy(m_manager, &StreamManager::streamError);

        QVariantMap config;
        config.insert(QStringLiteral("username"), QStringLiteral("player1"));

        QVERIFY(!m_manager->startStream(0, config));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(errorSpy.at(0).at(1).toString(), QStringLiteral("Instance already active"));

        // Original entry preserved
        QCOMPARE(m_manager->streams().size(), 1);
        QCOMPARE(m_manager->streamState(0), state);

        m_manager->m_streams.remove(0);
    }
}

// --- stopStream tests ---

void TestStreamManager::testStopStreamNonExistent()
{
    QVERIFY(!m_manager->stopStream(0));
    QVERIFY(!m_manager->stopStream(99));
}

void TestStreamManager::testStopStreamWithZeroPid()
{
    StreamManager::StreamEntry entry;
    entry.state = StreamManager::Waiting;
    entry.pid = 0;
    entry.instanceIndex = 0;
    entry.configDir = QString();
    m_manager->m_streams[0] = entry;

    QSignalSpy streamsSpy(m_manager, &StreamManager::streamsChanged);
    QSignalSpy stoppedSpy(m_manager, &StreamManager::streamStopped);

    QVERIFY(m_manager->stopStream(0));
    QVERIFY(m_manager->streams().isEmpty());
    QCOMPARE(streamsSpy.count(), 1);
    // streamStopped not emitted for zero-pid entries (no process was running)
    QCOMPARE(stoppedSpy.count(), 0);
}

void TestStreamManager::testStopAllEmpty()
{
    m_manager->stopAll();
    QVERIFY(m_manager->streams().isEmpty());
}

void TestStreamManager::testStopAllWithMultipleEntries()
{
    for (int i = 0; i < 3; ++i) {
        StreamManager::StreamEntry entry;
        entry.state = StreamManager::Waiting;
        entry.pid = 0;
        entry.instanceIndex = i;
        entry.configDir = QString();
        m_manager->m_streams[i] = entry;
    }

    QCOMPARE(m_manager->streams().size(), 3);
    m_manager->stopAll();
    QVERIFY(m_manager->streams().isEmpty());
}

// --- Helper unavailable path ---

void TestStreamManager::testStartStreamHelperUnavailable()
{
    // Pre-cleanup in case leftover from previous run
    QString configDir = SunshineConfig::defaultConfigDir(0);
    QDir(configDir).removeRecursively();

    QSignalSpy errorSpy(m_manager, &StreamManager::streamError);
    QSignalSpy startedSpy(m_manager, &StreamManager::streamStarted);

    QVariantMap config;
    config.insert(QStringLiteral("username"), QStringLiteral("testuser"));
    config.insert(QStringLiteral("streamBitrate"), 15000);

    bool result = m_manager->startStream(0, config);
    QVERIFY(!result);

    // Error signal emitted
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toInt(), 0);

    // streamStarted not emitted
    QCOMPARE(startedSpy.count(), 0);

    // No streams remain
    QVERIFY(m_manager->streams().isEmpty());
    QCOMPARE(m_manager->streamState(0), StreamManager::NotStarted);

    // Config dir cleaned up by error path
    QVERIFY(!QDir(configDir).exists());
}

// --- Startup timeout transition ---

void TestStreamManager::testStartupTimeoutTransition()
{
    // Simulate a stream in Waiting state with a startup timer
    StreamManager::StreamEntry entry;
    entry.state = StreamManager::Waiting;
    entry.pid = 99999;
    entry.instanceIndex = 0;
    entry.configDir = QStringLiteral("/tmp/test-config-0");
    m_manager->m_streams[0] = entry;

    QTimer *timer = new QTimer(m_manager);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, m_manager, &StreamManager::onStartupTimeout);
    m_manager->m_startupTimers[0] = timer;

    QSignalSpy startedSpy(m_manager, &StreamManager::streamStarted);
    QSignalSpy changedSpy(m_manager, &StreamManager::streamsChanged);

    QCOMPARE(m_manager->streamState(0), StreamManager::Waiting);
    QVERIFY(!m_manager->isStreaming(0));

    // Fire the timeout — should transition to Streaming and emit signals
    timer->start(0);
    QTest::qWait(50);

    QCOMPARE(m_manager->streamState(0), StreamManager::Streaming);
    QVERIFY(m_manager->isStreaming(0));
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.at(0).at(0).toInt(), 0);
    QCOMPARE(changedSpy.count(), 1);

    // Timer removed from map
    QVERIFY(!m_manager->m_startupTimers.contains(0));
}

// --- Auto-restart state machine ---

void TestStreamManager::testCrashDuringStartupBumpsPortAndSchedulesRestart()
{
    m_manager->setAutoRestart(true);
    StreamManager::StreamEntry entry;
    entry.instanceIndex = 0;
    entry.pid = 500001;
    entry.state = StreamManager::Waiting;
    entry.username = QStringLiteral("player1");
    entry.restartAttempts = 0;
    entry.lastConfig[QStringLiteral("sunshinePort")] = SunshineConfig::calculatePort(0);
    m_manager->m_streams[0] = entry;
    // A startup timer present => onHelperInstanceStopped treats it as a startup crash.
    auto *timer = new QTimer(m_manager);
    timer->setSingleShot(true);
    m_manager->m_startupTimers[0] = timer;

    const int originalPort = entry.lastConfig.value(QStringLiteral("sunshinePort")).toInt();
    QSignalSpy errorSpy(m_manager, &StreamManager::streamError);

    m_manager->onHelperInstanceStopped(QStringLiteral("player1"), 500001, QStringLiteral("crashed"));

    // Port bumped off the crash port...
    const int bumpedPort = m_manager->m_streams[0].lastConfig.value(QStringLiteral("sunshinePort")).toInt();
    QVERIFY2(bumpedPort != originalPort, qPrintable(QStringLiteral("port not bumped: %1").arg(bumpedPort)));
    // ...a tracked restart was scheduled (cancellable by stopStream)...
    QVERIFY(m_manager->m_restartTimers.contains(0));
    // ...and the startup timer was consumed.
    QVERIFY(!m_manager->m_startupTimers.contains(0));
    QCOMPARE(errorSpy.count(), 1);
}

void TestStreamManager::testCrashAfterStreamingKeepsPort()
{
    m_manager->setAutoRestart(true);
    StreamManager::StreamEntry entry;
    entry.instanceIndex = 1;
    entry.pid = 500002;
    entry.state = StreamManager::Streaming;
    entry.username = QStringLiteral("player2");
    entry.restartAttempts = 0;
    const int port = SunshineConfig::calculatePort(1);
    entry.lastConfig[QStringLiteral("sunshinePort")] = port;
    m_manager->m_streams[1] = entry;
    // No startup timer => not a startup crash => port must NOT be bumped.

    m_manager->onHelperInstanceStopped(QStringLiteral("player2"), 500002, QStringLiteral("crashed"));

    QCOMPARE(m_manager->m_streams[1].lastConfig.value(QStringLiteral("sunshinePort")).toInt(), port);
    QVERIFY(m_manager->m_restartTimers.contains(1));
}

void TestStreamManager::testAutoRestartDisabledRemovesImmediately()
{
    m_manager->setAutoRestart(false);
    StreamManager::StreamEntry entry;
    entry.instanceIndex = 2;
    entry.pid = 500003;
    entry.state = StreamManager::Streaming;
    entry.username = QStringLiteral("player3");
    m_manager->m_streams[2] = entry;

    QSignalSpy stoppedSpy(m_manager, &StreamManager::streamStopped);
    QSignalSpy errorSpy(m_manager, &StreamManager::streamError);

    m_manager->onHelperInstanceStopped(QStringLiteral("player3"), 500003, QStringLiteral("crashed"));

    QVERIFY(!m_manager->m_streams.contains(2));
    QVERIFY(!m_manager->m_restartTimers.contains(2));
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
}

void TestStreamManager::testStopStreamCancelsPendingRestart()
{
    m_manager->setAutoRestart(true);
    StreamManager::StreamEntry entry;
    entry.instanceIndex = 3;
    entry.pid = 500004;
    entry.state = StreamManager::Waiting;
    entry.username = QStringLiteral("player4");
    entry.restartAttempts = 0;
    entry.configDir = QStringLiteral("/tmp/couchplay-test-stopcancel");
    m_manager->m_streams[3] = entry;
    auto *timer = new QTimer(m_manager);
    timer->setSingleShot(true);
    m_manager->m_startupTimers[3] = timer;

    // Startup crash schedules a tracked restart timer...
    m_manager->onHelperInstanceStopped(QStringLiteral("player4"), 500004, QStringLiteral("crashed"));
    QVERIFY(m_manager->m_restartTimers.contains(3));

    // ...which stopStream must cancel (the fix: the restart timer is tracked in
    // m_restartTimers, not an anonymous singleShot that stopStream can't reach).
    m_manager->stopStream(3);
    QVERIFY(!m_manager->m_restartTimers.contains(3));
    QVERIFY(!m_manager->m_streams.contains(3));
}

QTEST_MAIN(TestStreamManager)
#include "test_streammanager.moc"
