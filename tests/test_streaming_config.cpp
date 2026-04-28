// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <QStandardPaths>
#include <QFile>

#include <KConfig>
#include <KConfigGroup>

#include "SessionManager.h"

#define KEY(x) QStringLiteral(x)

class TestStreamingConfig : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void testStreamingDefaults();
    void testStreamingConfigRoundTrip();
    void testBackwardCompatibilityOldProfile();
    void testSetInstanceStreamingConfig();

private:
    SessionManager *m_sessionManager = nullptr;
    QTemporaryDir *m_tempDir = nullptr;
};

void TestStreamingConfig::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
}

void TestStreamingConfig::cleanupTestCase()
{
    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestStreamingConfig::init()
{
    m_sessionManager = new SessionManager(this);
}

void TestStreamingConfig::cleanup()
{
    delete m_sessionManager;
    m_sessionManager = nullptr;
}

void TestStreamingConfig::testStreamingDefaults()
{
    QVariantMap config = m_sessionManager->getInstanceConfig(0);

    QCOMPARE(config.value(KEY("outputMode")).toString(), QStringLiteral("physical"));
    QCOMPARE(config.value(KEY("streamResolution")).toString(), QStringLiteral("1920x1080"));
    QCOMPARE(config.value(KEY("streamFps")).toInt(), 60);
    QCOMPARE(config.value(KEY("streamBitrate")).toInt(), 20000);
    QCOMPARE(config.value(KEY("streamCodec")).toString(), QStringLiteral("h264"));
    QCOMPARE(config.value(KEY("sunshinePort")).toInt(), 47989);
}

void TestStreamingConfig::testStreamingConfigRoundTrip()
{
    QVariantMap streamConfig;
    streamConfig.insert(KEY("outputMode"), QStringLiteral("streaming"));
    streamConfig.insert(KEY("streamResolution"), QStringLiteral("1280x720"));
    streamConfig.insert(KEY("streamFps"), 30);
    streamConfig.insert(KEY("streamBitrate"), 15000);
    streamConfig.insert(KEY("streamCodec"), QStringLiteral("h265"));
    streamConfig.insert(KEY("sunshinePort"), 48020);

    m_sessionManager->setInstanceConfig(0, streamConfig);

    bool saved = m_sessionManager->saveProfile(QStringLiteral("StreamRoundTrip"));
    QVERIFY(saved);

    m_sessionManager->newSession();
    QCOMPARE(m_sessionManager->instanceCount(), 2);

    bool loaded = m_sessionManager->loadProfile(QStringLiteral("StreamRoundTrip"));
    QVERIFY(loaded);

    QVariantMap loadedConfig = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(loadedConfig.value(KEY("outputMode")).toString(), QStringLiteral("streaming"));
    QCOMPARE(loadedConfig.value(KEY("streamResolution")).toString(), QStringLiteral("1280x720"));
    QCOMPARE(loadedConfig.value(KEY("streamFps")).toInt(), 30);
    QCOMPARE(loadedConfig.value(KEY("streamBitrate")).toInt(), 15000);
    QCOMPARE(loadedConfig.value(KEY("streamCodec")).toString(), QStringLiteral("h265"));
    QCOMPARE(loadedConfig.value(KEY("sunshinePort")).toInt(), 48020);
}

void TestStreamingConfig::testBackwardCompatibilityOldProfile()
{
    QString profilesDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                          + QStringLiteral("/profiles");
    QDir().mkpath(profilesDir);

    QString profilePath = profilesDir + QStringLiteral("/OldProfile.conf");
    QFile file(profilePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(
        "[General]\n"
        "name=OldProfile\n"
        "layout=horizontal\n"
        "instanceCount=2\n"
        "\n"
        "[Instance0]\n"
        "username=player1\n"
        "monitor=0\n"
        "internalWidth=1920\n"
        "internalHeight=1080\n"
        "outputWidth=960\n"
        "outputHeight=1080\n"
        "refreshRate=60\n"
        "scalingMode=fit\n"
        "filterMode=linear\n"
        "gameCommand=steam\n"
        "presetId=steam\n"
        "borderless=false\n"
        "\n"
        "[Instance1]\n"
        "username=player2\n"
        "monitor=0\n"
        "internalWidth=1920\n"
        "internalHeight=1080\n"
        "outputWidth=960\n"
        "outputHeight=1080\n"
        "refreshRate=60\n"
        "scalingMode=fit\n"
        "filterMode=linear\n"
        "gameCommand=steam\n"
        "presetId=steam\n"
        "borderless=false\n"
    );
    file.close();

    bool loaded = m_sessionManager->loadProfile(QStringLiteral("OldProfile"));
    QVERIFY(loaded);

    QVariantMap config0 = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(config0.value(KEY("username")).toString(), QStringLiteral("player1"));
    QCOMPARE(config0.value(KEY("outputMode")).toString(), QStringLiteral("physical"));
    QCOMPARE(config0.value(KEY("streamResolution")).toString(), QStringLiteral("1920x1080"));
    QCOMPARE(config0.value(KEY("streamFps")).toInt(), 60);
    QCOMPARE(config0.value(KEY("streamBitrate")).toInt(), 20000);
    QCOMPARE(config0.value(KEY("streamCodec")).toString(), QStringLiteral("h264"));
    QCOMPARE(config0.value(KEY("sunshinePort")).toInt(), 47989);

    QVariantMap config1 = m_sessionManager->getInstanceConfig(1);
    QCOMPARE(config1.value(KEY("username")).toString(), QStringLiteral("player2"));
    QCOMPARE(config1.value(KEY("outputMode")).toString(), QStringLiteral("physical"));

    file.remove();
}

void TestStreamingConfig::testSetInstanceStreamingConfig()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::instancesChanged);

    QVariantMap config;
    config.insert(KEY("outputMode"), QStringLiteral("streaming"));
    config.insert(KEY("streamCodec"), QStringLiteral("av1"));
    config.insert(KEY("sunshinePort"), 48050);

    m_sessionManager->setInstanceConfig(1, config);
    QCOMPARE(spy.count(), 1);

    QVariantMap retrieved = m_sessionManager->getInstanceConfig(1);
    QCOMPARE(retrieved.value(KEY("outputMode")).toString(), QStringLiteral("streaming"));
    QCOMPARE(retrieved.value(KEY("streamCodec")).toString(), QStringLiteral("av1"));
    QCOMPARE(retrieved.value(KEY("sunshinePort")).toInt(), 48050);
}

QTEST_MAIN(TestStreamingConfig)
#include "test_streaming_config.moc"
