// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "../src/core/StreamManager.h"
#include "../src/core/SunshineConfig.h"

namespace
{
const QString kIdentity = QStringLiteral("streammanager-test");
const QString kReadyLine = QStringLiteral("Info: Configuration UI available at [https://localhost:48020]");

// Restores an environment variable when the scope ends
class ScopedEnv
{
public:
    ScopedEnv(const char *name, const QByteArray &value)
        : m_name(name)
        , m_hadValue(qEnvironmentVariableIsSet(name))
        , m_old(qgetenv(name))
    {
        qputenv(name, value);
    }
    ~ScopedEnv()
    {
        if (m_hadValue) {
            qputenv(m_name, m_old);
        } else {
            qunsetenv(m_name);
        }
    }

private:
    const char *m_name;
    bool m_hadValue;
    QByteArray m_old;
};

// Stands in for sunshine. `$1` is the conf; `$2 == --creds` writes the credentials file.
// FAKE_SUNSHINE_MODE selects the behaviour; every launch is appended to <stateDir>/runs.
const char *kFakeSunshine = R"SH(#!/bin/sh
conf="$1"
dir=$(dirname "$conf")
if [ "$2" = "--creds" ]; then
    echo "$@" > "$dir/creds-args"
    echo '{}' > "$dir/credentials.json"
    exit 0
fi
log=$(sed -n 's/^log_path = //p' "$conf")
scheme=https:/
echo run >> "$dir/runs"
case "$FAKE_SUNSHINE_MODE" in
ready)
    echo "Info: Configuration UI available at [${scheme}/localhost:48020]" >> "$log"
    exec sleep 30
    ;;
gamepad)
    echo "Info: Configuration UI available at [${scheme}/localhost:48020]" >> "$log"
    echo "Info: Gamepad 0 will be DualSense (PS5) controller" >> "$log"
    exec sleep 30
    ;;
crash)
    echo "Info: Configuration UI available at [${scheme}/localhost:48020]" >> "$log"
    exit 1
    ;;
fatal)
    echo "Error: Unable to find display or encoder during startup." >> "$log"
    exec sleep 30
    ;;
nokwin)
    echo "Error: Invalid value for capture: kwin" >> "$log"
    exec sleep 30
    ;;
exit)
    echo "Error: something broke" >> "$log"
    exit 3
    ;;
silent)
    exec sleep 30
    ;;
esac
exit 9
)SH";
}

class TestStreamManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void testMissingSunshinePath();
    void testReadyLineStartsStreaming();
    void testCredentialsCreatedOnce();
    void testStreamsMapEntry();
    void testFatalEncoderLine();
    void testKwinUnsupported();
    void testExitBeforeReadyDoesNotRestart();
    void testStartupTimeout();
    void testCrashAfterReadyRestartsThenErrors();
    void testCrashWithoutAutoRestart();
    void testStopStream();
    void testStopAll();
    void testGamepadAnnounced();

private:
    QString scriptPath() const { return m_scriptDir->filePath(QStringLiteral("fake-sunshine")); }
    StreamSpec spec(int index = 1) const;
    QString stateDir() const { return SunshineConfig::stateDirectory(kIdentity); }
    int runCount() const;

    QTemporaryDir *m_scriptDir = nullptr;
};

void TestStreamManager::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_scriptDir = new QTemporaryDir;
    QVERIFY(m_scriptDir->isValid());

    QFile script(scriptPath());
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(kFakeSunshine);
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath(),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                      | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
                                      | QFileDevice::ExeOther));
}

void TestStreamManager::init()
{
    QDir(stateDir()).removeRecursively();
}

void TestStreamManager::cleanup()
{
    QDir(stateDir()).removeRecursively();
}

StreamSpec TestStreamManager::spec(int index) const
{
    StreamSpec result;
    result.instanceIndex = index;
    result.identityKey = kIdentity;
    result.outputName = QStringLiteral("Virtual-couchplay-p2");
    result.sinkName = QStringLiteral("couchplay-stream-p2");
    result.port = 48019;
    return result;
}

int TestStreamManager::runCount() const
{
    QFile file(stateDir() + QStringLiteral("/runs"));
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
}

void TestStreamManager::testMissingSunshinePath()
{
    StreamManager manager;
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);

    QVERIFY(!manager.startStream(1, spec()));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(errorSpy.at(0).at(1).toString(), QStringLiteral("Sunshine is not installed; set its path in Settings"));
    QCOMPARE(manager.streamState(1), StreamManager::Error);
    QVERIFY(!manager.hasOwnedPids());
}

void TestStreamManager::testReadyLineStartsStreaming()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "ready");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);

    QVERIFY(manager.startStream(1, spec()));
    QCOMPARE(manager.streamState(1), StreamManager::Waiting);
    QVERIFY(manager.hasOwnedPid(1));
    QVERIFY(manager.hasOwnedPids());

    QTRY_COMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(manager.streamState(1), StreamManager::Streaming);
    QVERIFY(manager.isStreaming(1));
    QVERIFY(errorSpy.isEmpty());

    manager.stopAll();
    QTRY_VERIFY(!manager.hasOwnedPids());
}

void TestStreamManager::testCredentialsCreatedOnce()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "ready");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);
    QSignalSpy stoppedSpy(&manager, &StreamManager::streamStopped);

    QVERIFY(manager.startStream(1, spec()));
    QVERIFY(QFile::exists(stateDir() + QStringLiteral("/credentials.json")));

    QFile args(stateDir() + QStringLiteral("/creds-args"));
    QVERIFY(args.open(QIODevice::ReadOnly));
    const QString line = QString::fromUtf8(args.readAll()).trimmed();
    args.close();
    QVERIFY(line.contains(QStringLiteral("--creds couchplay ")));
    QVERIFY(line.startsWith(stateDir() + QStringLiteral("/sunshine.conf")));

    QTRY_COMPARE(startedSpy.count(), 1);
    manager.stopAll();
    QTRY_COMPARE(stoppedSpy.count(), 1);

    // credentials.json exists now: --creds must not run again
    QVERIFY(QFile::remove(stateDir() + QStringLiteral("/creds-args")));
    QVERIFY(manager.startStream(1, spec()));
    QVERIFY(!QFile::exists(stateDir() + QStringLiteral("/creds-args")));
    manager.stopAll();
    QTRY_VERIFY(!manager.hasOwnedPids());
}

void TestStreamManager::testStreamsMapEntry()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "ready");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    QSignalSpy changedSpy(&manager, &StreamManager::streamsChanged);
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);

    QVERIFY(manager.startStream(1, spec()));
    QVERIFY(changedSpy.count() > 0);
    QTRY_COMPARE(startedSpy.count(), 1);

    const QVariantList streams = manager.streams();
    QCOMPARE(streams.size(), 1);
    const QVariantMap entry = streams.at(0).toMap();
    QCOMPARE(entry.value(QStringLiteral("instanceIndex")).toInt(), 1);
    QCOMPARE(entry.value(QStringLiteral("state")).toInt(), static_cast<int>(StreamManager::Streaming));
    QCOMPARE(entry.value(QStringLiteral("port")).toInt(), 48019);
    QCOMPARE(entry.value(QStringLiteral("sunshineName")).toString(), QStringLiteral("CouchPlay – Player 2 (streammanager-test)"));
    QCOMPARE(entry.value(QStringLiteral("outputName")).toString(), QStringLiteral("Virtual-couchplay-p2"));
    QVERIFY(entry.value(QStringLiteral("error")).toString().isEmpty());

    manager.stopAll();
    QTRY_VERIFY(!manager.hasOwnedPids());
    QTRY_VERIFY(manager.streams().isEmpty());
}

void TestStreamManager::testFatalEncoderLine()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "fatal");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);
    QSignalSpy stoppedSpy(&manager, &StreamManager::streamStopped);

    QVERIFY(manager.startStream(1, spec()));
    QTRY_COMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(1).toString(),
             QStringLiteral("Sunshine could not capture Virtual-couchplay-p2 or found no encoder"));
    QCOMPARE(manager.streamState(1), StreamManager::Error);

    // The process is killed and never restarted
    QTRY_COMPARE(stoppedSpy.count(), 1);
    QVERIFY(!manager.hasOwnedPid(1));
    QTest::qWait(500);
    QCOMPARE(runCount(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(startedSpy.isEmpty());
}

void TestStreamManager::testKwinUnsupported()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "nokwin");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);

    QVERIFY(manager.startStream(1, spec()));
    QTRY_COMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.at(0).at(1).toString(),
             QStringLiteral("Sunshine at %1 does not support KWin capture; install Sunshine 2026.516 or newer")
                 .arg(scriptPath()));
    QTRY_VERIFY(!manager.hasOwnedPids());
}

void TestStreamManager::testExitBeforeReadyDoesNotRestart()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "exit");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    manager.setRestartDelay(20);
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);
    QSignalSpy stoppedSpy(&manager, &StreamManager::streamStopped);

    QVERIFY(manager.startStream(1, spec()));
    QTRY_COMPARE(errorSpy.count(), 1);
    const QString message = errorSpy.at(0).at(1).toString();
    QVERIFY2(message.contains(QStringLiteral("exited before it was ready")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("exit code 3")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("something broke")), qPrintable(message));
    QTRY_COMPARE(stoppedSpy.count(), 1);

    QTest::qWait(400);
    QCOMPARE(runCount(), 1);
    QVERIFY(startedSpy.isEmpty());
    QCOMPARE(manager.streamState(1), StreamManager::Error);
}

void TestStreamManager::testStartupTimeout()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "silent");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    manager.setStartupTimeout(400);
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);
    QSignalSpy stoppedSpy(&manager, &StreamManager::streamStopped);

    QVERIFY(manager.startStream(1, spec()));
    QTRY_COMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.at(0).at(1).toString().contains(QStringLiteral("did not become ready")));
    QTRY_COMPARE(stoppedSpy.count(), 1);
    QCOMPARE(runCount(), 1);
}

void TestStreamManager::testCrashAfterReadyRestartsThenErrors()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "crash");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    manager.setRestartDelay(30);
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);
    QSignalSpy stoppedSpy(&manager, &StreamManager::streamStopped);

    QVERIFY(manager.startStream(1, spec()));
    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 15000);
    QTRY_COMPARE(stoppedSpy.count(), 1);

    // Initial launch plus MAX_RESTARTS restarts, each reaching Ready
    QCOMPARE(runCount(), 1 + StreamManager::MAX_RESTARTS);
    QCOMPARE(startedSpy.count(), 1 + StreamManager::MAX_RESTARTS);
    QCOMPARE(manager.streamState(1), StreamManager::Error);
    QVERIFY(!manager.hasOwnedPid(1));
}

void TestStreamManager::testCrashWithoutAutoRestart()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "crash");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    manager.setRestartDelay(30);
    manager.setAutoRestart(false);
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);

    QVERIFY(manager.startStream(1, spec()));
    QTRY_COMPARE(errorSpy.count(), 1);
    QCOMPARE(startedSpy.count(), 1);
    QTest::qWait(300);
    QCOMPARE(runCount(), 1);
}

void TestStreamManager::testStopStream()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "ready");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);
    QSignalSpy errorSpy(&manager, &StreamManager::streamError);
    QSignalSpy stoppedSpy(&manager, &StreamManager::streamStopped);

    QVERIFY(!manager.stopStream(1));
    QVERIFY(manager.startStream(1, spec()));
    QTRY_COMPARE(startedSpy.count(), 1);

    QVERIFY(manager.stopStream(1));
    QTRY_COMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(0).toInt(), 1);
    QVERIFY(!manager.hasOwnedPid(1));
    QCOMPARE(manager.streamState(1), StreamManager::NotStarted);
    QVERIFY(errorSpy.isEmpty());

    // A requested stop never restarts
    QTest::qWait(300);
    QCOMPARE(runCount(), 1);
    QCOMPARE(stoppedSpy.count(), 1);
}

void TestStreamManager::testStopAll()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "ready");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    QSignalSpy startedSpy(&manager, &StreamManager::streamStarted);
    QSignalSpy stoppedSpy(&manager, &StreamManager::streamStopped);

    // Same identity for both is fine for this purpose: the second launch reuses the state dir
    QVERIFY(manager.startStream(0, spec(0)));
    StreamSpec second = spec(1);
    second.port = 48049;
    QVERIFY(manager.startStream(1, second));
    QTRY_COMPARE(startedSpy.count(), 2);

    manager.stopAll();
    QTRY_COMPARE(stoppedSpy.count(), 2);
    QVERIFY(!manager.hasOwnedPids());
}

void TestStreamManager::testGamepadAnnounced()
{
    ScopedEnv mode("FAKE_SUNSHINE_MODE", "gamepad");
    StreamManager manager;
    manager.setProgramOverride(scriptPath());
    QSignalSpy gamepadSpy(&manager, &StreamManager::gamepadAnnounced);

    QVERIFY(manager.startStream(1, spec()));
    QTRY_COMPARE(gamepadSpy.count(), 1);
    QCOMPARE(gamepadSpy.at(0).at(0).toInt(), 1);

    manager.stopAll();
    QTRY_VERIFY(!manager.hasOwnedPids());
}

QTEST_MAIN(TestStreamManager)
#include "test_streammanager.moc"
