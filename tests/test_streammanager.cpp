// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QStandardPaths>

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
    void testStartStreamAlreadyStreaming();

    // stopStream tests
    void testStopStreamNonExistent();
    void testStopStreamWithZeroPid();
    void testStopAllEmpty();
    void testStopAllWithMultipleEntries();

    // Helper unavailable path (assumes no CouchPlayHelper D-Bus service)
    void testStartStreamHelperUnavailable();

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

void TestStreamManager::testStartStreamAlreadyStreaming()
{
    StreamManager::StreamEntry entry;
    entry.state = StreamManager::Streaming;
    entry.pid = 12345;
    entry.instanceIndex = 0;
    m_manager->m_streams[0] = entry;

    QSignalSpy errorSpy(m_manager, &StreamManager::streamError);

    QVariantMap config;
    config.insert(QStringLiteral("username"), QStringLiteral("player1"));

    QVERIFY(!m_manager->startStream(0, config));
    QCOMPARE(errorSpy.count(), 1);

    // Original entry preserved
    QCOMPARE(m_manager->streams().size(), 1);
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

QTEST_MAIN(TestStreamManager)
#include "test_streammanager.moc"
