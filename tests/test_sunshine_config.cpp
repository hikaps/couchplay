// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTest>

#include "../src/core/SunshineConfig.h"

namespace
{
QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

// Parses "key = value" lines
QMap<QString, QString> readConf(const QString &path)
{
    QMap<QString, QString> result;
    for (const QString &line : readLines(path)) {
        const qsizetype eq = line.indexOf(QStringLiteral(" = "));
        if (eq > 0) {
            result.insert(line.left(eq), line.mid(eq + 3));
        }
    }
    return result;
}

StreamSpec makeSpec()
{
    StreamSpec spec;
    spec.instanceIndex = 1;
    spec.identityKey = QStringLiteral("player2");
    spec.outputName = QStringLiteral("Virtual-couchplay-p2");
    spec.sinkName = QStringLiteral("couchplay-stream-p2");
    spec.port = 48019;
    spec.bitrate = 15000;
    return spec;
}
}

class TestSunshineConfig : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testPortCalculation();
    void testStateDirectorySanitized();
    void testEnsureStateDirectoryPermissions();
    void testSecretStableAndPrivate();
    void testGenerateConfigKeys();
    void testGenerateConfigWithoutSink();
    void testCustomDisplayName();
    void testCodecMapping_data();
    void testCodecMapping();
    void testAppsJson();
    void testConfigRewrittenEveryTime();
    void testValueInjectionStripped();
    void testCredentialArguments();
    void testAllocatePortSkipsBusyPort();
    void testAllocatePortExhausted();
    void testPairedClients();
    void testPairedClientsMissingOrMalformed();
};

void TestSunshineConfig::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void TestSunshineConfig::testPairedClients()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("paired-test"));
    QFile file(dir + QStringLiteral("/sunshine_state.json"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(R"({"root":{"uniqueid":"abc","named_devices":[)"
               R"({"name":"Phone","uuid":"u1"},{"name":"No uuid","uuid":""},{"name":"Deck","uuid":"u2"}]}})");
    file.close();

    const QVariantList clients = SunshineConfig::pairedClients(QStringLiteral("paired-test"));
    QCOMPARE(clients.size(), 2);
    QCOMPARE(clients.at(0).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Phone"));
    QCOMPARE(clients.at(1).toMap().value(QStringLiteral("uuid")).toString(), QStringLiteral("u2"));
    QCOMPARE(SunshineConfig::pairedClientCount(QStringLiteral("paired-test")), 2);
}

void TestSunshineConfig::testPairedClientsMissingOrMalformed()
{
    // Unknown user: no file, and the state directory must not be created
    const QString missing = SunshineConfig::stateDirectory(QStringLiteral("paired-never-created"));
    QDir(missing).removeRecursively();
    QVERIFY(SunshineConfig::pairedClients(QStringLiteral("paired-never-created")).isEmpty());
    QCOMPARE(SunshineConfig::pairedClientCount(QStringLiteral("paired-never-created")), 0);
    QVERIFY(!QFileInfo::exists(missing));

    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("paired-malformed"));
    QFile file(dir + QStringLiteral("/sunshine_state.json"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{not json");
    file.close();
    QCOMPARE(SunshineConfig::pairedClientCount(QStringLiteral("paired-malformed")), 0);

    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(R"({"root":{"uniqueid":"x"}})");
    file.close();
    QCOMPARE(SunshineConfig::pairedClientCount(QStringLiteral("paired-malformed")), 0);
}

void TestSunshineConfig::testPortCalculation()
{
    QCOMPARE(SunshineConfig::calculatePort(0), SunshineConfig::BASE_PORT);
    QCOMPARE(SunshineConfig::calculatePort(2), SunshineConfig::BASE_PORT + 2 * SunshineConfig::PORT_SPACING);
    QCOMPARE(SunshineConfig::calculatePort(1, 50000), 50000 + SunshineConfig::PORT_SPACING);
    QCOMPARE(SunshineConfig::calculatePort(1000), SunshineConfig::MAX_PORT);
    QCOMPARE(SunshineConfig::calculatePort(0, 100), SunshineConfig::MIN_PORT);
}

void TestSunshineConfig::testStateDirectorySanitized()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sunshine/");
    QCOMPARE(SunshineConfig::stateDirectory(QStringLiteral("player1")), root + QStringLiteral("player1"));
    QCOMPARE(SunshineConfig::stateDirectory(QString()), root + QStringLiteral("_desktop"));
    QCOMPARE(SunshineConfig::stateDirectory(QStringLiteral("..")), root + QStringLiteral("_desktop"));
    QCOMPARE(SunshineConfig::stateDirectory(QStringLiteral("a/../b c")), root + QStringLiteral("a_.._b_c"));
}

void TestSunshineConfig::testEnsureStateDirectoryPermissions()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("perm-test"));
    QVERIFY(!dir.isEmpty());
    QVERIFY(QFileInfo(dir).isDir());
    QCOMPARE(QFileInfo(dir).permissions() & 0x7077, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

void TestSunshineConfig::testSecretStableAndPrivate()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("secret-test"));
    QVERIFY(!dir.isEmpty());
    QFile::remove(dir + QStringLiteral("/couchplay.secret"));

    const QString first = SunshineConfig::secret(dir);
    QCOMPARE(first.size(), 32);
    for (const QChar ch : first) {
        QVERIFY(ch.isLetterOrNumber() && ch.unicode() < 128);
    }
    QCOMPARE(SunshineConfig::secret(dir), first);

    QCOMPARE(QFileInfo(dir + QStringLiteral("/couchplay.secret")).permissions() & 0x7077,
             QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    // A different state dir gets a different secret
    const QString other = SunshineConfig::ensureStateDirectory(QStringLiteral("secret-test-other"));
    QFile::remove(other + QStringLiteral("/couchplay.secret"));
    QVERIFY(SunshineConfig::secret(other) != first);
}

void TestSunshineConfig::testGenerateConfigKeys()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("keys-test"));
    const StreamSpec spec = makeSpec();
    const QString conf = SunshineConfig::generateConfig(spec, dir);
    QCOMPARE(conf, dir + QStringLiteral("/sunshine.conf"));

    const QMap<QString, QString> values = readConf(conf);
    const QStringList expectedKeys{QStringLiteral("sunshine_name"),
                                   QStringLiteral("port"),
                                   QStringLiteral("capture"),
                                   QStringLiteral("output_name"),
                                   QStringLiteral("audio_sink"),
                                   QStringLiteral("file_state"),
                                   QStringLiteral("credentials_file"),
                                   QStringLiteral("pkey"),
                                   QStringLiteral("cert"),
                                   QStringLiteral("file_apps"),
                                   QStringLiteral("log_path"),
                                   QStringLiteral("keyboard"),
                                   QStringLiteral("mouse"),
                                   QStringLiteral("controller"),
                                   QStringLiteral("max_bitrate"),
                                   QStringLiteral("origin_web_ui_allowed"),
                                   QStringLiteral("upnp")};
    QCOMPARE(QStringList(values.keys()), [&]() {
        QStringList sorted = expectedKeys;
        sorted.sort();
        return sorted;
    }());

    QCOMPARE(values.value(QStringLiteral("sunshine_name")), QStringLiteral("CouchPlay – Player 2 (player2)"));
    QCOMPARE(values.value(QStringLiteral("port")), QStringLiteral("48019"));
    QCOMPARE(values.value(QStringLiteral("capture")), QStringLiteral("kwin"));
    QCOMPARE(values.value(QStringLiteral("output_name")), QStringLiteral("Virtual-couchplay-p2"));
    QCOMPARE(values.value(QStringLiteral("audio_sink")), QStringLiteral("couchplay-stream-p2"));
    QCOMPARE(values.value(QStringLiteral("file_state")), dir + QStringLiteral("/sunshine_state.json"));
    QCOMPARE(values.value(QStringLiteral("credentials_file")), dir + QStringLiteral("/credentials.json"));
    QCOMPARE(values.value(QStringLiteral("pkey")), dir + QStringLiteral("/pkey.pem"));
    QCOMPARE(values.value(QStringLiteral("cert")), dir + QStringLiteral("/cert.pem"));
    QCOMPARE(values.value(QStringLiteral("file_apps")), dir + QStringLiteral("/apps.json"));
    QCOMPARE(values.value(QStringLiteral("log_path")), dir + QStringLiteral("/sunshine.log"));
    QCOMPARE(values.value(QStringLiteral("keyboard")), QStringLiteral("enabled"));
    QCOMPARE(values.value(QStringLiteral("mouse")), QStringLiteral("enabled"));
    QCOMPARE(values.value(QStringLiteral("controller")), QStringLiteral("enabled"));
    QCOMPARE(values.value(QStringLiteral("max_bitrate")), QStringLiteral("15000"));
    QCOMPARE(values.value(QStringLiteral("origin_web_ui_allowed")), QStringLiteral("pc"));
    QCOMPARE(values.value(QStringLiteral("upnp")), QStringLiteral("disabled"));

    QVERIFY(!values.contains(QStringLiteral("encoder")));
    QVERIFY(!values.contains(QStringLiteral("sink")));
    QVERIFY(!values.contains(QStringLiteral("hevc_mode")));
    QVERIFY(!values.contains(QStringLiteral("av1_mode")));
}

void TestSunshineConfig::testGenerateConfigWithoutSink()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("nosink-test"));
    StreamSpec spec = makeSpec();
    spec.sinkName.clear();
    const QMap<QString, QString> values = readConf(SunshineConfig::generateConfig(spec, dir));
    QVERIFY(!values.contains(QStringLiteral("audio_sink")));
    QVERIFY(!values.contains(QStringLiteral("sink")));
}

void TestSunshineConfig::testCustomDisplayName()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("name-test"));
    StreamSpec spec = makeSpec();
    spec.displayName = QStringLiteral("Living Room");
    const QMap<QString, QString> values = readConf(SunshineConfig::generateConfig(spec, dir));
    QCOMPARE(values.value(QStringLiteral("sunshine_name")), QStringLiteral("Living Room"));
    QCOMPARE(SunshineConfig::displayName(spec), QStringLiteral("Living Room"));
}

void TestSunshineConfig::testCodecMapping_data()
{
    QTest::addColumn<QString>("codec");
    QTest::addColumn<QString>("hevc");
    QTest::addColumn<QString>("av1");

    QTest::newRow("auto") << QStringLiteral("auto") << QString() << QString();
    QTest::newRow("h264") << QStringLiteral("h264") << QStringLiteral("1") << QStringLiteral("1");
    QTest::newRow("hevc") << QStringLiteral("hevc") << QString() << QStringLiteral("1");
    QTest::newRow("av1") << QStringLiteral("av1") << QString() << QString();
}

void TestSunshineConfig::testCodecMapping()
{
    QFETCH(QString, codec);
    QFETCH(QString, hevc);
    QFETCH(QString, av1);

    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("codec-test"));
    StreamSpec spec = makeSpec();
    spec.codec = codec;
    const QMap<QString, QString> values = readConf(SunshineConfig::generateConfig(spec, dir));

    QCOMPARE(values.value(QStringLiteral("hevc_mode")), hevc);
    QCOMPARE(values.contains(QStringLiteral("hevc_mode")), !hevc.isEmpty());
    QCOMPARE(values.value(QStringLiteral("av1_mode")), av1);
    QCOMPARE(values.contains(QStringLiteral("av1_mode")), !av1.isEmpty());
}

void TestSunshineConfig::testAppsJson()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("apps-test"));
    QVERIFY(!SunshineConfig::generateConfig(makeSpec(), dir).isEmpty());

    QFile file(dir + QStringLiteral("/apps.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QVERIFY(doc.isObject());
    QVERIFY(doc.object().value(QStringLiteral("env")).toObject().isEmpty());
    const QJsonArray apps = doc.object().value(QStringLiteral("apps")).toArray();
    QCOMPARE(apps.size(), 1);
    QCOMPARE(apps.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("CouchPlay"));
    QCOMPARE(apps.at(0).toObject().value(QStringLiteral("image-path")).toString(), QStringLiteral("desktop.png"));
}

void TestSunshineConfig::testConfigRewrittenEveryTime()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("rewrite-test"));
    StreamSpec spec = makeSpec();
    QVERIFY(!SunshineConfig::generateConfig(spec, dir).isEmpty());
    spec.port = 48049;
    spec.outputName = QStringLiteral("HDMI-A-1");
    const QMap<QString, QString> values = readConf(SunshineConfig::generateConfig(spec, dir));
    QCOMPARE(values.value(QStringLiteral("port")), QStringLiteral("48049"));
    QCOMPARE(values.value(QStringLiteral("output_name")), QStringLiteral("HDMI-A-1"));
}

void TestSunshineConfig::testValueInjectionStripped()
{
    const QString dir = SunshineConfig::ensureStateDirectory(QStringLiteral("inject-test"));
    StreamSpec spec = makeSpec();
    spec.outputName = QStringLiteral("DP-1\nencoder = software");
    const QString conf = SunshineConfig::generateConfig(spec, dir);
    const QMap<QString, QString> values = readConf(conf);
    QVERIFY(!values.contains(QStringLiteral("encoder")));
    QCOMPARE(values.value(QStringLiteral("output_name")), QStringLiteral("DP-1encoder = software"));
}

void TestSunshineConfig::testCredentialArguments()
{
    const QStringList args = SunshineConfig::credentialArguments(QStringLiteral("/x/sunshine.conf"), QStringLiteral("s3cret"));
    QCOMPARE(args,
             (QStringList{QStringLiteral("/x/sunshine.conf"),
                          QStringLiteral("--creds"),
                          QStringLiteral("couchplay"),
                          QStringLiteral("s3cret")}));
}

void TestSunshineConfig::testAllocatePortSkipsBusyPort()
{
    // Find a preferred port whose TCP base port can be held by this test
    for (int preferred = 51000; preferred < 52000; preferred += 100) {
        QTcpServer holder;
        if (!holder.listen(QHostAddress::Any, static_cast<quint16>(preferred))) {
            continue;
        }
        const int allocated = SunshineConfig::allocatePort(preferred);
        QVERIFY(allocated != preferred);
        if (allocated == -1) {
            QSKIP("No free port block near the preferred port on this machine");
        }
        QCOMPARE(allocated, preferred + SunshineConfig::PORT_SPACING);
        return;
    }
    QSKIP("Could not hold a TCP port for the test");
}

void TestSunshineConfig::testAllocatePortExhausted()
{
    for (int preferred = 53000; preferred < 54000; preferred += 100) {
        QTcpServer holder;
        if (!holder.listen(QHostAddress::Any, static_cast<quint16>(preferred))) {
            continue;
        }
        QCOMPARE(SunshineConfig::allocatePort(preferred, 1), -1);
        return;
    }
    QSKIP("Could not hold a TCP port for the test");
}

QTEST_MAIN(TestSunshineConfig)
#include "test_sunshine_config.moc"
