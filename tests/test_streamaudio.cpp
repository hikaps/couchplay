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
// load-module prints 100 + N, records the module in $FAKE_PACTL_LOG.modules and fails when
// N == $FAKE_PACTL_FAIL_ON; unload-module drops the module with that number from the list;
// list short sinks cats $FAKE_PACTL_LOG.sinks, list short modules cats $FAKE_PACTL_LOG.modules,
// and any list fails when $FAKE_PACTL_FAIL_LIST is non-empty.
const char *kFakePactl = R"PACTL(#!/bin/sh
echo "$@" >> "$FAKE_PACTL_LOG"
state="$FAKE_PACTL_LOG.default"
modules="$FAKE_PACTL_LOG.modules"
tab=$(printf '\t')
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
    printf '%s\tmodule-null-sink\t%s %s\t\n' "$((100 + n))" "$3" "$4" >> "$modules"
    echo $((100 + n))
    ;;
unload-module)
    if [ -f "$modules" ]; then
        grep -v "^$2$tab" "$modules" > "$modules.new"
        mv "$modules.new" "$modules"
    fi
    ;;
list)
    if [ -n "$FAKE_PACTL_FAIL_LIST" ]; then
        exit 1
    fi
    if [ "$3" = "modules" ]; then f="$modules"; else f="$FAKE_PACTL_LOG.sinks"; fi
    [ -f "$f" ] && cat "$f"
    ;;
esac
exit 0
)PACTL";

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
    void testEnsureSinksListFailureReturnsFalse();
    void testEnsureSinksLoadFailureReturnsFalse();
    void testEnsureSinksSuccessReturnsTrue();
    void testTeardownUnloadsByNameNotStaleIndex();
    void testLoadFailureRollsBack();
    void testUnpreparedIndexHasNoSink();

private:
    QStringList calls() const;
    void resetLog()
    {
        QFile::remove(m_log);
        for (const char *suffix : {".default", ".sinks", ".modules"}) {
            QFile::remove(m_log + QLatin1String(suffix));
        }
    }
    void writeState(const char *suffix, const QByteArray &content)
    {
        QFile file(m_log + QLatin1String(suffix));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
    }

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
    // After the 3 prepare calls: module lookup, unloads in load order, then default query and restore
    QCOMPARE(all.mid(3),
             (QStringList{QStringLiteral("list short modules"),
                          QStringLiteral("unload-module 102"),
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

    QVERIFY(audio.ensureSinks());

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

    QVERIFY(audio.ensureSinks());
    QCOMPARE(calls().mid(2), (QStringList{QStringLiteral("list short sinks")}));
    QFile::remove(m_log + QStringLiteral(".sinks"));
    audio.teardown();

    // Inactive: nothing is run at all
    resetLog();
    StreamAudio idle;
    idle.setProgramOverride(m_script);
    QVERIFY(idle.ensureSinks());
    QVERIFY(calls().isEmpty());
}

void TestStreamAudio::testEnsureSinksListFailureReturnsFalse()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({1}));

    {
        ScopedEnv failList("FAKE_PACTL_FAIL_LIST", "1");
        QVERIFY(!audio.ensureSinks());
    }
    // No reload was attempted blindly after the failed listing
    QCOMPARE(calls().mid(2), (QStringList{QStringLiteral("list short sinks")}));
    QVERIFY(audio.isActive());
    audio.teardown();
}

void TestStreamAudio::testEnsureSinksLoadFailureReturnsFalse()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({1, 2}));

    // pipewire-pulse restarted: both sinks and their modules are gone, and the reload of player 2 fails
    writeState(".sinks", "7\talsa_output.speakers\tPipeWire\n");
    QFile::remove(m_log + QStringLiteral(".modules"));
    {
        ScopedEnv failLoad("FAKE_PACTL_FAIL_ON", "2");
        QVERIFY(!audio.ensureSinks());
    }

    // Player 3's sink was reloaded (module 103); the failed one left nothing to unload
    audio.teardown();
    const QStringList all = calls();
    QVERIFY(all.contains(QStringLiteral("unload-module 103")));
    QVERIFY(!all.contains(QStringLiteral("unload-module 102")));
}

void TestStreamAudio::testEnsureSinksSuccessReturnsTrue()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({1, 2}));

    // Everything vanished: both sinks are reloaded
    writeState(".sinks", "7\talsa_output.speakers\tPipeWire\n");
    QVERIFY(audio.ensureSinks());
    const QStringList all = calls().mid(3);
    QCOMPARE(all.size(), 3);
    QCOMPARE(all.at(0), QStringLiteral("list short sinks"));
    QVERIFY(all.at(1).startsWith(QStringLiteral("load-module module-null-sink sink_name=couchplay-stream-p2 ")));
    QVERIFY(all.at(2).startsWith(QStringLiteral("load-module module-null-sink sink_name=couchplay-stream-p3 ")));
    audio.teardown();
}

void TestStreamAudio::testTeardownUnloadsByNameNotStaleIndex()
{
    resetLog();
    ScopedEnv log("FAKE_PACTL_LOG", m_log.toUtf8());
    ScopedEnv def("FAKE_PACTL_DEFAULT", "alsa_output.speakers");
    ScopedEnv fail("FAKE_PACTL_FAIL_ON", "none");

    StreamAudio audio;
    audio.setProgramOverride(m_script);
    QVERIFY(audio.prepare({1, 2})); // loaded as modules 102 and 103

    // pipewire-pulse restarted and renumbered everything; module 102 and 103 now belong to others
    writeState(".modules",
               "102\tmodule-null-sink\tsink_name=sink-sunshine-stereo sink_properties=device.description=Steam_Streaming\t\n"
               "103\tmodule-loopback\tsink=couchplay-stream-p2\t\n"
               "31\tmodule-null-sink\tsink_name=couchplay-stream-p2 sink_properties=device.description=CouchPlay_Player_2_stream\t\n"
               "32\tmodule-null-sink\tsink_name=couchplay-stream-p3 sink_properties=device.description=CouchPlay_Player_3_stream\t\n"
               "40\tmodule-null-sink\tsink_name=couchplay-stream-p9\t\n");

    audio.teardown();
    QVERIFY(!audio.isActive());

    const QStringList unloads = calls().filter(QStringLiteral("unload-module"));
    QCOMPARE(unloads, (QStringList{QStringLiteral("unload-module 31"), QStringLiteral("unload-module 32")}));
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
