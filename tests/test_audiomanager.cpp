// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

#define private public
#include "AudioManager.h"
#undef private

class TestAudioManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void testConstructionDefaults();
    void testPipeWireDetection();
    void testPulseAudioFallback();
    void testMultiUserConfiguredRequiresAcl();
    void testConfigurationChangedSignal();
    void testAudioServerChangedSignal();
    void testCheckSocketAclNonexistent();
    void testCheckSocketAclWithFakeSocket();

private:
    AudioManager *m_manager = nullptr;
    QTemporaryDir m_tempDir;
};

void TestAudioManager::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(m_tempDir.isValid());
}

void TestAudioManager::cleanupTestCase()
{
}

void TestAudioManager::init()
{
    m_manager = nullptr;
}

void TestAudioManager::cleanup()
{
    delete m_manager;
    m_manager = nullptr;
}

void TestAudioManager::testConstructionDefaults()
{
    m_manager = new AudioManager();

    // No pipewire-0 socket in the isolated test runtime directory
    QVERIFY(!m_manager->isMultiUserConfigured());

    const QString server = m_manager->audioServer();
    QVERIFY2(server == QStringLiteral("pipewire") || server == QStringLiteral("pulseaudio"),
             qPrintable(QStringLiteral("Unexpected audio server: %1").arg(server)));
}

void TestAudioManager::testPipeWireDetection()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QDir().mkpath(runtimeDir);

    const QString socketPath = runtimeDir + QStringLiteral("/pipewire-0");
    const bool socketExisted = QFile::exists(socketPath);
    if (!socketExisted) {
        QFile f(socketPath);
        QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(QStringLiteral("Failed to create %1").arg(socketPath)));
        f.close();
    }

    m_manager = new AudioManager();
    QCOMPARE(m_manager->audioServer(), QStringLiteral("pipewire"));

    if (!socketExisted) {
        QFile::remove(socketPath);
    }
}

void TestAudioManager::testPulseAudioFallback()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString socketPath = runtimeDir + QStringLiteral("/pipewire-0");
    QFile::remove(socketPath);

    m_manager = new AudioManager();
    QCOMPARE(m_manager->audioServer(), QStringLiteral("pulseaudio"));
}

void TestAudioManager::testMultiUserConfiguredRequiresAcl()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QDir().mkpath(runtimeDir);
    const QString socketPath = runtimeDir + QStringLiteral("/pipewire-0");
    {
        QFile f(socketPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
    }

    m_manager = new AudioManager();

    // Even though pipewire-0 exists, multiUserConfigured should be false
    // because the test environment lacks the couchplay group ACL on the socket.
    // This verifies that mere file existence is insufficient (Issue #1/#2).
    QVERIFY(!m_manager->isMultiUserConfigured());

    QFile::remove(socketPath);
}

void TestAudioManager::testConfigurationChangedSignal()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QDir().mkpath(runtimeDir);
    const QString socketPath = runtimeDir + QStringLiteral("/pipewire-0");
    {
        QFile f(socketPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
    }

    m_manager = new AudioManager();

    QSignalSpy spy(m_manager, &AudioManager::configurationChanged);
    QVERIFY(spy.isValid());

    m_manager->checkConfiguration();
    QCOMPARE(spy.count(), 0);

    QFile::remove(socketPath);
    m_manager->checkConfiguration();
    QCOMPARE(spy.count(), 0);
}

void TestAudioManager::testAudioServerChangedSignal()
{
    const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString socketPath = runtimeDir + QStringLiteral("/pipewire-0");

    QFile::remove(socketPath);
    m_manager = new AudioManager();

    QSignalSpy spy(m_manager, &AudioManager::audioServerChanged);
    QVERIFY(spy.isValid());

    m_manager->checkConfiguration();
    QCOMPARE(spy.count(), 0);

    QDir().mkpath(runtimeDir);
    {
        QFile f(socketPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
    }
    m_manager->checkConfiguration();
    QCOMPARE(spy.count(), 1);

    QFile::remove(socketPath);
}

void TestAudioManager::testCheckSocketAclNonexistent()
{
    m_manager = new AudioManager();

    const QString fakePath = QStringLiteral("/tmp/couchplay-test-nonexistent-socket-39847");
    QVERIFY(!QFile::exists(fakePath));

    QVERIFY(!m_manager->checkSocketAcl(fakePath));
}

void TestAudioManager::testCheckSocketAclWithFakeSocket()
{
    m_manager = new AudioManager();

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString fakeSocket = tempDir.path() + QStringLiteral("/pipewire-0");
    {
        QFile f(fakeSocket);
        QVERIFY2(f.open(QIODevice::WriteOnly), "Failed to create fake socket file");
        f.write("test");
        f.close();
    }

    const bool result = m_manager->checkSocketAcl(fakeSocket);

    QVERIFY(!result);
}

QTEST_GUILESS_MAIN(TestAudioManager)
#include "test_audiomanager.moc"
