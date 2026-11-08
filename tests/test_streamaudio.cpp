// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QFile>
#include <QObject>
#include <QTemporaryDir>
#include <QTest>

#include "../src/core/StreamAudio.h"

namespace
{
// Fake pactl: logs argv (one call per line) to $FAKE_PACTL_LOG.
// get-default-sink prints $FAKE_PACTL_DEFAULT (or the last set-default-sink);
// load-module prints 100 + N and fails when N == $FAKE_PACTL_FAIL_ON.
const char *kFakePactl = R"(#!/bin/sh
echo "$@" >> "$FAKE_PACTL_LOG"
state="$FAKE_PACTL_LOG.default"
case "$1" in
get-default-sink)
    if [ -f "$state" ]; then cat "$state"; else echo "$FAKE_PACTL_DEFAULT"; fi
    ;;
set-default-sink)
    echo "$2" > "$state"
    ;;
load-module)
    n=$(echo "$3" | sed 's/.*-p//')
    if [ "$n" = "$FAKE_PACTL_FAIL_ON" ]; then
        exit 1
    fi
    echo $((100 + n))
    ;;
unload-module)
    ;;
list)
    [ -f "$FAKE_PACTL_LOG.sinks" ] && cat "$FAKE_PACTL_LOG.sinks"
    ;;
esac
exit 0
)";

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
}

class TestStreamAudio : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testPrepareLoadsSinksPerPlayer();
    void testTeardownUnloadsAndRestoresDefault();
    void testTeardownIdempotent();
    void testSinkMovedDefaultRestored();
    void testEnsureSinksReloadsMissing();
    void testEnsureSinksNoopWhenPresent();
    void testLoadFailureRollsBack();
    void testUnpreparedIndexHasNoSink();

private:
    QStringList calls() const;
    void resetLog() { QFile::remove(m_log); QFile::remove(m_log + QStringLiteral(".default")); }

    QTemporaryDir m_dir;
    QString m_script;
    QString m_log;
};

void TestStreamAudio::initTestCase()
{
    QVERIFY(m_dir.isValid());
    m_script = m_dir.filePath(QStringLiteral("fake-pactl"));
    m_log = m_dir.filePath(QStringLiteral("calls.log"));

    QFile script(m_script);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(kFakePactl);
    script.close();
    QVERIFY(QFile::setPermissions(m_script,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
}

QStringList TestStreamAudio::calls() const
{
    QFile file(m_log);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

void TestStreamAudio::testPrepareLoadsSinksPerPlayer()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(!audio.isActive());
    QString error;
    QVERIFY(audio.prepare({1, 2}, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(audio.isActive());

    QCOMPARE(audio.defaultSink(), QStringLiteral("alsa_output.speakers"));
    QCOMPARE(audio.streamSink(1), QStringLiteral("couchplay-stream-p2"));
    QCOMPARE(audio.streamSink(2), QStringLiteral("couchplay-stream-p3"));

    const QStringList expected{
        QStringLiteral("get-default-sink"),
        QStringLiteral("load-module module-null-sink sink_name=couchplay-stream-p2 "
                       "sink_properties=device.description=CouchPlay_Player_2_stream"),
        QStringLiteral("load-module module-null-sink sink_name=couchplay-stream-p3 "
                       "sink_properties=device.description=CouchPlay_Player_3_stream"),
    };
    QCOMPARE(calls(), expected);

    audio.teardown();
}

void TestStreamAudio::testTeardownUnloadsAndRestoresDefault()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({1, 2}));

    // Sunshine moved the default to a stream sink
    QVERIFY(QFile::exists(m_log));
    QFile state(m_log + QStringLiteral(".default"));
    QVERIFY(state.open(QIODevice::WriteOnly));
    state.write("couchplay-stream-p2\n");
    state.close();

    audio.teardown();
    QVERIFY(!audio.isActive());
    QVERIFY(audio.streamSink(1).isEmpty());

    const QStringList all = calls();
    // After the 3 prepare calls: unloads in load order, then default query and restore
    QCOMPARE(all.mid(3),
             (QStringList{QStringLiteral("unload-module 102"),
                          QStringLiteral("unload-module 103"),
                          QStringLiteral("get-default-sink"),
                          QStringLiteral("set-default-sink alsa_output.speakers")}));
}

void TestStreamAudio::testTeardownIdempotent()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({0}));
    audio.teardown();
    const int count = calls().size();
    audio.teardown();
    QCOMPARE(calls().size(), count);
}

void TestStreamAudio::testSinkMovedDefaultRestored()
{
    // Default unchanged: no set-default-sink needed
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({0}));
    audio.teardown();

    for (const QString &call : calls()) {
        QVERIFY2(!call.startsWith(QStringLiteral("set-default-sink")), qPrintable(call));
    }
}

void TestStreamAudio::testLoadFailureRollsBack()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "3"); // player 3 = index 2

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QString error;
    QVERIFY(!audio.prepare({0, 1, 2}, &error));
    QCOMPARE(error, QStringLiteral("Player 3: could not create stream audio sink"));
    QVERIFY(!audio.isActive());
    QVERIFY(audio.streamSink(0).isEmpty());

    const QStringList all = calls();
    QVERIFY(all.contains(QStringLiteral("unload-module 101")));
    QVERIFY(all.contains(QStringLiteral("unload-module 102")));
    QVERIFY(all.indexOf(QStringLiteral("unload-module 101")) < all.indexOf(QStringLiteral("unload-module 102")));
}

void TestStreamAudio::testEnsureSinksReloadsMissing()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({1, 2}));

    // pipewire-pulse restarted: only player 3's sink survived
    QFile sinks(m_log + QStringLiteral(".sinks"));
    QVERIFY(sinks.open(QIODevice::WriteOnly));
    sinks.write("7\talsa_output.speakers\tPipeWire\n8\tcouchplay-stream-p3\tPipeWire\n");
    sinks.close();

    audio.ensureSinks();

    const QStringList all = calls().mid(3);
    QCOMPARE(all,
             (QStringList{QStringLiteral("list short sinks"),
                          QStringLiteral("load-module module-null-sink sink_name=couchplay-stream-p2 "
                                         "sink_properties=device.description=CouchPlay_Player_2_stream")}));
    QFile::remove(m_log + QStringLiteral(".sinks"));
    audio.teardown();
}

void TestStreamAudio::testEnsureSinksNoopWhenPresent()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({1}));

    QFile sinks(m_log + QStringLiteral(".sinks"));
    QVERIFY(sinks.open(QIODevice::WriteOnly));
    sinks.write("8\tcouchplay-stream-p2\tPipeWire\n");
    sinks.close();

    audio.ensureSinks();
    QCOMPARE(calls().mid(2), (QStringList{QStringLiteral("list short sinks")}));
    QFile::remove(m_log + QStringLiteral(".sinks"));
    audio.teardown();

    // Inactive: nothing is run at all
    resetLog();
    StreamAudio idle;
    idle.setProgramOverride(m_script);
    idle.ensureSinks();
    QVERIFY(calls().isEmpty());
}

void TestStreamAudio::testUnpreparedIndexHasNoSink()
{
    StreamAudio audio;
    QVERIFY(audio.streamSink(0).isEmpty());
    QVERIFY(audio.defaultSink().isEmpty());
    QVERIFY(!audio.isActive());
    audio.teardown(); // must not crash or run anything
}

QTEST_MAIN(TestStreamAudio)
#include "test_streamaudio.moc"
