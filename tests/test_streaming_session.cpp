// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
 #include <QSet>
#include "SessionManager.h"
#include "SunshineConfig.h"

#define KEY(x) QStringLiteral(x)

class TestStreamingSession : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Mixed mode profile tests
    void testMixedPhysicalStreamingProfile();
    void testAllStreamingProfile();
    void testAllPhysicalProfile();
    void testMixedProfileSaveLoad();
    void testStreamingConfigToSunshineConfig();
    void testEffectiveSunshinePortsAreDistinct();
    void testStreamConfigValuesPreserved();

private:
    SessionManager *m_sessionManager = nullptr;
    QTemporaryDir *m_tempDir = nullptr;
};

void TestStreamingSession::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
}

void TestStreamingSession::cleanupTestCase()
{
    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestStreamingSession::init()
{
    m_sessionManager = new SessionManager(this);
}

void TestStreamingSession::cleanup()
{
    delete m_sessionManager;
    m_sessionManager = nullptr;
}

// --- Mixed mode profile tests ---

void TestStreamingSession::testMixedPhysicalStreamingProfile()
{
    m_sessionManager->setInstanceCount(2);

    QVariantMap config0;
    config0.insert(KEY("display"), QStringLiteral("main"));
    config0.insert(KEY("username"), QStringLiteral("player1"));
    m_sessionManager->setInstanceConfig(0, config0);

    QVariantMap config1;
    config1.insert(KEY("display"), QStringLiteral("virtual"));
    config1.insert(KEY("virtualResolution"), QStringLiteral("1280x720"));
    config1.insert(KEY("streamBitrate"), 10000);
    config1.insert(KEY("streamCodec"), QStringLiteral("hevc"));
    config1.insert(KEY("username"), QStringLiteral("player2"));
    m_sessionManager->setInstanceConfig(1, config1);

    QVariantMap retrieved0 = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(retrieved0.value(KEY("display")).toString(), QStringLiteral("main"));
    QCOMPARE(retrieved0.value(KEY("streamToMoonlight")).toBool(), false);
    QCOMPARE(retrieved0.value(KEY("username")).toString(), QStringLiteral("player1"));

    QVariantMap retrieved1 = m_sessionManager->getInstanceConfig(1);
    QCOMPARE(retrieved1.value(KEY("display")).toString(), QStringLiteral("virtual"));
    QCOMPARE(retrieved1.value(KEY("streamToMoonlight")).toBool(), true);
    QCOMPARE(retrieved1.value(KEY("virtualResolution")).toString(), QStringLiteral("1280x720"));
    QCOMPARE(retrieved1.value(KEY("streamBitrate")).toInt(), 10000);
    QCOMPARE(retrieved1.value(KEY("streamCodec")).toString(), QStringLiteral("hevc"));
    QCOMPARE(retrieved1.value(KEY("username")).toString(), QStringLiteral("player2"));
}

void TestStreamingSession::testAllStreamingProfile()
{
    m_sessionManager->setInstanceCount(3);

    for (int i = 0; i < 3; ++i) {
        QVariantMap config;
        config.insert(KEY("display"), QStringLiteral("virtual"));
        config.insert(KEY("username"), QStringLiteral("streamer%1").arg(i));
        config.insert(KEY("streamCodec"), QStringLiteral("hevc"));
        config.insert(KEY("streamBitrate"), 15000);
        m_sessionManager->setInstanceConfig(i, config);
    }

    for (int i = 0; i < 3; ++i) {
        QVariantMap retrieved = m_sessionManager->getInstanceConfig(i);
        QCOMPARE(retrieved.value(KEY("display")).toString(), QStringLiteral("virtual"));
        QCOMPARE(retrieved.value(KEY("streamCodec")).toString(), QStringLiteral("hevc"));
        QCOMPARE(retrieved.value(KEY("streamBitrate")).toInt(), 15000);
    }
}

void TestStreamingSession::testAllPhysicalProfile()
{
    m_sessionManager->setInstanceCount(2);

    for (int i = 0; i < 2; ++i) {
        QVariantMap retrieved = m_sessionManager->getInstanceConfig(i);
        QCOMPARE(retrieved.value(KEY("display")).toString(), QStringLiteral("main"));
    }
}

void TestStreamingSession::testMixedProfileSaveLoad()
{
    m_sessionManager->setInstanceCount(2);

    QVariantMap config0;
    config0.insert(KEY("display"), QStringLiteral("main"));
    config0.insert(KEY("username"), QStringLiteral("localplayer"));
    m_sessionManager->setInstanceConfig(0, config0);

    QVariantMap config1;
    config1.insert(KEY("display"), QStringLiteral("virtual"));
    config1.insert(KEY("virtualResolution"), QStringLiteral("3840x2160"));
    config1.insert(KEY("streamBitrate"), 50000);
    config1.insert(KEY("streamCodec"), QStringLiteral("av1"));
    config1.insert(KEY("sunshinePort"), 48019);
    config1.insert(KEY("username"), QStringLiteral("remoteplayer"));
    m_sessionManager->setInstanceConfig(1, config1);

    bool saved = m_sessionManager->saveProfile(QStringLiteral("MixedTest"));
    QVERIFY(saved);

    m_sessionManager->newSession();
    QCOMPARE(m_sessionManager->instanceCount(), 2);

    bool loaded = m_sessionManager->loadProfile(QStringLiteral("MixedTest"));
    QVERIFY(loaded);

    QVariantMap loaded0 = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(loaded0.value(KEY("display")).toString(), QStringLiteral("main"));
    QCOMPARE(loaded0.value(KEY("username")).toString(), QStringLiteral("localplayer"));

    QVariantMap loaded1 = m_sessionManager->getInstanceConfig(1);
    QCOMPARE(loaded1.value(KEY("display")).toString(), QStringLiteral("virtual"));
    QCOMPARE(loaded1.value(KEY("virtualResolution")).toString(), QStringLiteral("3840x2160"));
    QCOMPARE(loaded1.value(KEY("streamBitrate")).toInt(), 50000);
    QCOMPARE(loaded1.value(KEY("streamCodec")).toString(), QStringLiteral("av1"));
    QCOMPARE(loaded1.value(KEY("sunshinePort")).toInt(), 48019);
    QCOMPARE(loaded1.value(KEY("username")).toString(), QStringLiteral("remoteplayer"));
}

void TestStreamingSession::testStreamingConfigToSunshineConfig()
{
    QTemporaryDir configDir;
    QVERIFY(configDir.isValid());

    m_sessionManager->setInstanceCount(2);

    QVariantMap streamConfig;
    streamConfig.insert(KEY("display"), QStringLiteral("virtual"));
    streamConfig.insert(KEY("streamBitrate"), 25000);
    streamConfig.insert(KEY("streamCodec"), QStringLiteral("hevc"));
    streamConfig.insert(KEY("sunshinePort"), 48019);
    streamConfig.insert(KEY("username"), QStringLiteral("streamer"));
    m_sessionManager->setInstanceConfig(1, streamConfig);

    QVariantMap retrieved = m_sessionManager->getInstanceConfig(1);

    StreamSpec spec;
    spec.instanceIndex = 1;
    spec.identityKey = retrieved.value(KEY("username")).toString();
    spec.outputName = QStringLiteral("Virtual-couchplay-p2");
    spec.port = SunshineConfig::calculatePort(0, retrieved.value(KEY("sunshinePort")).toInt());
    spec.bitrate = retrieved.value(KEY("streamBitrate")).toInt();
    spec.codec = retrieved.value(KEY("streamCodec")).toString();

    QString configPath = SunshineConfig::generateConfig(spec, configDir.path());
    QVERIFY(!configPath.isEmpty());

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(file.readAll());
    file.close();

    QVERIFY(content.contains(QStringLiteral("port = 48019")));
    QVERIFY(content.contains(QStringLiteral("max_bitrate = 25000")));
    QVERIFY(content.contains(QStringLiteral("av1_mode = 1")));
    QVERIFY(content.contains(QStringLiteral("output_name = Virtual-couchplay-p2")));
    QVERIFY(content.contains(QStringLiteral("capture = kwin")));
    QVERIFY(content.contains(QStringLiteral("sunshine_name = CouchPlay – Player 2 (streamer)")));
}

void TestStreamingSession::testEffectiveSunshinePortsAreDistinct()
{
    m_sessionManager->setInstanceCount(3);

    QSet<int> ports;
    for (int i = 0; i < 3; ++i) {
        ports.insert(m_sessionManager->effectiveSunshinePort(i));
    }
    QCOMPARE(ports.size(), 3);

    QVariantMap config = m_sessionManager->getInstanceConfig(1);
    config.insert(KEY("sunshinePort"), 50000);
    m_sessionManager->setInstanceConfig(1, config);
    QCOMPARE(m_sessionManager->effectiveSunshinePort(1), SunshineConfig::calculatePort(1, 50000));
}

void TestStreamingSession::testStreamConfigValuesPreserved()
{
    m_sessionManager->setInstanceCount(1);

    QVariantMap config;
    config.insert(KEY("display"), QStringLiteral("virtual"));
    config.insert(KEY("virtualResolution"), QStringLiteral("2560x1440"));
    config.insert(KEY("streamBitrate"), 40000);
    config.insert(KEY("streamCodec"), QStringLiteral("av1"));
    config.insert(KEY("sunshinePort"), 48049);
    config.insert(KEY("username"), QStringLiteral("poweruser"));
    m_sessionManager->setInstanceConfig(0, config);

    QVariantMap retrieved = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(retrieved.value(KEY("display")).toString(), QStringLiteral("virtual"));
    QCOMPARE(retrieved.value(KEY("virtualResolution")).toString(), QStringLiteral("2560x1440"));
    QCOMPARE(retrieved.value(KEY("streamBitrate")).toInt(), 40000);
    QCOMPARE(retrieved.value(KEY("streamCodec")).toString(), QStringLiteral("av1"));
    QCOMPARE(retrieved.value(KEY("sunshinePort")).toInt(), 48049);
    QCOMPARE(retrieved.value(KEY("username")).toString(), QStringLiteral("poweruser"));
}

QTEST_MAIN(TestStreamingSession)
#include "test_streaming_session.moc"
