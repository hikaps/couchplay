// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QCryptographicHash>
#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTest>
#include <QDebug>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "../src/core/SunshineConfig.h"

class TestSunshineConfig : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void testPortCalculation();
    void testPortCalculationWithCustomBase();
    void testGenerateConfigCreatesFiles();
    void testGeneratedConfigContent();
    void testDifferentInstancesProduceDifferentPorts();
    void testDifferentInstancesProduceDifferentPaths();
    void testConfigParsesAsValidINI();
    void testAppsJsonIsValid();
    void testDefaultCredentials();
    void testCustomCredentials();
    void testOutputNameIncluded();
    void testBitrateIncluded();
    void testConfigValueInjectionStripped();
    void testEmptyConfigDirFails();
    void testNegativeIndexFails();
    void testDefaultConfigDir();
    void testDirectoryPermissions();

private:
    QTemporaryDir *m_tempDir = nullptr;
};


void TestSunshineConfig::initTestCase()
{
    m_tempDir = new QTemporaryDir(QDir::tempPath() + QStringLiteral("/couchplay-sunshine-test-XXXXXX"));
    QVERIFY(m_tempDir->isValid());
}

void TestSunshineConfig::cleanupTestCase()
{
    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestSunshineConfig::cleanup()
{
    if (!m_tempDir)
        return;
    QDir dir(m_tempDir->path());
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        QDir subDir(dir.filePath(entry));
        const QStringList files = subDir.entryList(QDir::Files);
        for (const QString &file : files) {
            QFile::remove(subDir.filePath(file));
        }
        subDir.rmdir(subDir.absolutePath());
    }
}

void TestSunshineConfig::testPortCalculation()
{
    QCOMPARE(SunshineConfig::calculatePort(0), 47989);
    QCOMPARE(SunshineConfig::calculatePort(1), 48019);
    QCOMPARE(SunshineConfig::calculatePort(2), 48049);
    QCOMPARE(SunshineConfig::calculatePort(3), 48079);
}

void TestSunshineConfig::testPortCalculationWithCustomBase()
{
    QCOMPARE(SunshineConfig::calculatePort(0, 50000), 50000);
    QCOMPARE(SunshineConfig::calculatePort(1, 50000), 50030);
    QCOMPARE(SunshineConfig::calculatePort(2, 50000), 50060);
}

void TestSunshineConfig::testGenerateConfigCreatesFiles()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/instance-0");
    QVariantMap config;

    QString result = SunshineConfig::generateConfig(config, 0, configDir);

    QVERIFY(!result.isEmpty());
    QCOMPARE(result, configDir + QStringLiteral("/sunshine.conf"));
    QVERIFY(QFile::exists(result));
    QVERIFY(QFile::exists(configDir + QStringLiteral("/apps.json")));
}

void TestSunshineConfig::testGeneratedConfigContent()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/content-test");
    QVariantMap config;
    config.insert(QStringLiteral("outputName"), QStringLiteral("2"));
    config.insert(QStringLiteral("username"), QStringLiteral("player1"));
    config.insert(QStringLiteral("password"), QStringLiteral("s3cret"));
    config.insert(QStringLiteral("streamBitrate"), 15000);

    QString configPath = SunshineConfig::generateConfig(config, 1, configDir);
    QVERIFY(!configPath.isEmpty());

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(file.readAll());
    file.close();

    QVERIFY(content.contains(QStringLiteral("port = 48019")));
    QVERIFY(content.contains(QStringLiteral("output_name = 2")));
    QVERIFY(!content.contains(QStringLiteral("username = ")));
    QVERIFY(!content.contains(QStringLiteral("password = ")));
    QVERIFY(content.contains(QStringLiteral("max_bitrate = 15000")));
    QVERIFY(content.contains(QStringLiteral("gamepad = auto")));
    QVERIFY(content.contains(QStringLiteral("controller = enabled")));
    QVERIFY(content.contains(QStringLiteral("keyboard = enabled")));
    QVERIFY(content.contains(QStringLiteral("mouse = enabled")));
    QVERIFY(content.contains(QStringLiteral("sunshine_name = CouchPlay Player 1")));
    QVERIFY(content.contains(QStringLiteral("origin_web_ui_allowed = pc")));
    QVERIFY(content.contains(configDir + QStringLiteral("/apps.json")));
    QVERIFY(content.contains(configDir + QStringLiteral("/credentials.json")));
    QVERIFY(content.contains(configDir + QStringLiteral("/sunshine.log")));

    // Credentials should be in a separate JSON file, not in sunshine.conf
    QVERIFY(QFile::exists(configDir + QStringLiteral("/credentials.json")));
}

void TestSunshineConfig::testDifferentInstancesProduceDifferentPorts()
{
    QString dir0 = m_tempDir->path() + QStringLiteral("/diff-port-0");
    QString dir1 = m_tempDir->path() + QStringLiteral("/diff-port-1");

    QVariantMap config;
    QString path0 = SunshineConfig::generateConfig(config, 0, dir0);
    QString path1 = SunshineConfig::generateConfig(config, 1, dir1);
    QVERIFY(!path0.isEmpty());
    QVERIFY(!path1.isEmpty());

    QFile f0(path0);
    QVERIFY(f0.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content0 = QString::fromUtf8(f0.readAll());

    QFile f1(path1);
    QVERIFY(f1.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content1 = QString::fromUtf8(f1.readAll());

    QVERIFY(content0.contains(QStringLiteral("port = 47989")));
    QVERIFY(content1.contains(QStringLiteral("port = 48019")));
}

void TestSunshineConfig::testDifferentInstancesProduceDifferentPaths()
{
    QString dir0 = m_tempDir->path() + QStringLiteral("/diff-path-0");
    QString dir1 = m_tempDir->path() + QStringLiteral("/diff-path-1");

    QVariantMap config;
    QString path0 = SunshineConfig::generateConfig(config, 0, dir0);
    QString path1 = SunshineConfig::generateConfig(config, 1, dir1);

    QVERIFY(path0 != path1);
    QVERIFY(path0.contains(QStringLiteral("diff-path-0")));
    QVERIFY(path1.contains(QStringLiteral("diff-path-1")));
}

void TestSunshineConfig::testConfigParsesAsValidINI()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/ini-valid");
    QVariantMap config;
    config.insert(QStringLiteral("outputName"), QStringLiteral("1"));

    QString configPath = SunshineConfig::generateConfig(config, 0, configDir);
    QVERIFY(!configPath.isEmpty());

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QStringLiteral("\n"));
    file.close();

    for (const QString &line : lines) {
        if (line.isEmpty())
            continue;

        QVERIFY2(line.contains(QStringLiteral("=")),
                 qPrintable(QStringLiteral("Line is not valid INI key=value: ") + line));
    }
}

void TestSunshineConfig::testAppsJsonIsValid()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/apps-json");
    QVariantMap config;

    SunshineConfig::generateConfig(config, 0, configDir);

    QFile appsFile(configDir + QStringLiteral("/apps.json"));
    QVERIFY(appsFile.open(QIODevice::ReadOnly | QIODevice::Text));
    QByteArray content = appsFile.readAll();
    appsFile.close();

    QVERIFY(content.contains(QByteArrayLiteral("\"env\"")));
    QVERIFY(content.contains(QByteArrayLiteral("\"apps\"")));
    QVERIFY(content.contains(QByteArrayLiteral("\"Desktop\"")));
}

void TestSunshineConfig::testDefaultCredentials()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/default-creds");
    QVariantMap config;

    QString configPath = SunshineConfig::generateConfig(config, 0, configDir);
    QVERIFY(!configPath.isEmpty());

    // credentials.json should contain the default username
    QFile credsFile(configDir + QStringLiteral("/credentials.json"));
    QVERIFY(credsFile.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(credsFile.readAll());
    credsFile.close();

    QCOMPARE(doc.object().value(QStringLiteral("username")).toString(), QStringLiteral("couchplay"));
    QVERIFY(!doc.object().value(QStringLiteral("salt")).toString().isEmpty());
    QVERIFY(!doc.object().value(QStringLiteral("password")).toString().isEmpty());
}

void TestSunshineConfig::testCustomCredentials()
{
    // Self-consistency / regression guard ONLY: recomputes the hash with the
    // SAME algorithm SunshineConfig uses. This catches accidental changes to
    // the hash scheme, NOT whether Sunshine accepts it -- that requires a real
    // pairing (test_sunshine_integration covers config-key acceptance; hash
    // validation against real Sunshine is a deferred follow-up).
    QString configDir = m_tempDir->path() + QStringLiteral("/custom-creds");
    QVariantMap config;
    config.insert(QStringLiteral("username"), QStringLiteral("admin"));
    config.insert(QStringLiteral("password"), QStringLiteral("hunter2"));

    QString configPath = SunshineConfig::generateConfig(config, 0, configDir);
    QVERIFY(!configPath.isEmpty());

    QFile credsFile(configDir + QStringLiteral("/credentials.json"));
    QVERIFY(credsFile.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(credsFile.readAll());
    credsFile.close();

    QCOMPARE(doc.object().value(QStringLiteral("username")).toString(), QStringLiteral("admin"));
    // Verify the hash matches reversed SHA256("hunter2" + salt)
    QString salt = doc.object().value(QStringLiteral("salt")).toString();
    QByteArray expectedRaw = QCryptographicHash::hash((QStringLiteral("hunter2") + salt).toUtf8(), QCryptographicHash::Sha256);
    std::reverse(expectedRaw.begin(), expectedRaw.end());
    QString expectedHash = QString::fromLatin1(expectedRaw.toHex()).toUpper();
    QCOMPARE(doc.object().value(QStringLiteral("password")).toString(), expectedHash);
}

void TestSunshineConfig::testOutputNameIncluded()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/output-name");
    QVariantMap config;
    config.insert(QStringLiteral("outputName"), QStringLiteral("3"));

    QString configPath = SunshineConfig::generateConfig(config, 0, configDir);
    QVERIFY(!configPath.isEmpty());

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(file.readAll());
    file.close();

    QVERIFY(content.contains(QStringLiteral("output_name = 3")));
}

void TestSunshineConfig::testBitrateIncluded()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/bitrate");
    QVariantMap config;
    config.insert(QStringLiteral("streamBitrate"), 25000);

    QString configPath = SunshineConfig::generateConfig(config, 0, configDir);
    QVERIFY(!configPath.isEmpty());

    QFile file(configPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = QString::fromUtf8(file.readAll());
    file.close();

    QVERIFY(content.contains(QStringLiteral("max_bitrate = 25000")));
}

void TestSunshineConfig::testConfigValueInjectionStripped()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/inject");
    QVariantMap config;
    // CR/LF in a value must not inject an extra config line into sunshine.conf
    // (it is read by Sunshine, which the helper launches).
    config.insert(QStringLiteral("outputName"), QStringLiteral("0\nfake_injected_key = pwned\rsecond = bad"));

    SunshineConfig::generateConfig(config, 0, configDir);

    QFile file(configDir + QStringLiteral("/sunshine.conf"));
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(file.readAll());
    file.close();

    QVERIFY2(!content.contains(QLatin1String("\nfake_injected_key")),
             "CR/LF injection survived into sunshine.conf");
    QVERIFY2(!content.contains(QLatin1String("fake_injected_key = pwned\n")),
             "injected key became its own config line");
    // The legitimate key is still emitted, value kept to the pre-newline text.
    QVERIFY(content.contains(QStringLiteral("output_name = 0")));
}

void TestSunshineConfig::testEmptyConfigDirFails()
{
    QVariantMap config;
    QString result = SunshineConfig::generateConfig(config, 0, QString());
    QVERIFY(result.isEmpty());
}

void TestSunshineConfig::testNegativeIndexFails()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/negative");
    QVariantMap config;
    QString result = SunshineConfig::generateConfig(config, -1, configDir);
    QVERIFY(result.isEmpty());
}

void TestSunshineConfig::testDefaultConfigDir()
{
    QCOMPARE(SunshineConfig::defaultConfigDir(0), QStringLiteral("/tmp/couchplay-sunshine-0"));
    QCOMPARE(SunshineConfig::defaultConfigDir(1), QStringLiteral("/tmp/couchplay-sunshine-1"));
    QCOMPARE(SunshineConfig::defaultConfigDir(5), QStringLiteral("/tmp/couchplay-sunshine-5"));
}

void TestSunshineConfig::testDirectoryPermissions()
{
    QString configDir = m_tempDir->path() + QStringLiteral("/perms-test");
    QVariantMap config;

    QString result = SunshineConfig::generateConfig(config, 0, configDir);
    QVERIFY(!result.isEmpty());
    QVERIFY(QDir(configDir).exists());

    QFileInfo dirInfo(configDir);
    QFile::Permissions perms = dirInfo.permissions();
    QVERIFY(perms & QFileDevice::ReadOwner);
    QVERIFY(perms & QFileDevice::WriteOwner);
    QVERIFY(perms & QFileDevice::ExeOwner);
    QVERIFY(perms & QFileDevice::ReadGroup);
    QVERIFY(perms & QFileDevice::ExeGroup);
    QVERIFY(perms & QFileDevice::ReadOther);
    QVERIFY(perms & QFileDevice::ExeOther);
}

QTEST_MAIN(TestSunshineConfig)
#include "test_sunshine_config.moc"
