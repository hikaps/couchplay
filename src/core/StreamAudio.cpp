// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "StreamAudio.h"

#include "HostProcess.h"

#include <QDebug>
#include <QProcess>

namespace
{
constexpr int kPactlTimeoutMs = 5000;
}

bool StreamAudio::runPactl(const QStringList &args, QString *output) const
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    HostProcess::configure(process, m_programOverride.isEmpty() ? QStringLiteral("pactl") : m_programOverride, args);
    process.start();
    if (!process.waitForStarted(kPactlTimeoutMs)) {
        qWarning() << "StreamAudio: could not start pactl:" << process.errorString();
        return false;
    }
    if (!process.waitForFinished(kPactlTimeoutMs)) {
        qWarning() << "StreamAudio: pactl timed out:" << args;
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    if (output) {
        *output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning() << "StreamAudio: pactl" << args << "failed:" << QString::fromUtf8(process.readAllStandardError()).trimmed();
        return false;
    }
    return true;
}

bool StreamAudio::prepare(const QList<int> &streamingIndices, QString *error)
{
    if (m_active) {
        teardown();
    }

    // Best effort: without a recorded default there is simply nothing to restore
    QString defaultSink;
    if (runPactl({QStringLiteral("get-default-sink")}, &defaultSink)) {
        m_defaultSink = defaultSink;
    } else {
        m_defaultSink.clear();
    }

    for (const int index : streamingIndices) {
        const int player = index + 1;
        const QString sinkName = QStringLiteral("couchplay-stream-p%1").arg(player);
        QString output;
        bool ok = runPactl({QStringLiteral("load-module"),
                            QStringLiteral("module-null-sink"),
                            QStringLiteral("sink_name=%1").arg(sinkName),
                            QStringLiteral("sink_properties=device.description=CouchPlay_Player_%1_stream").arg(player)},
                           &output);
        uint moduleIndex = 0;
        if (ok) {
            moduleIndex = output.toUInt(&ok);
        }
        if (!ok) {
            if (error) {
                *error = QStringLiteral("Player %1: could not create stream audio sink").arg(player);
            }
            teardown();
            return false;
        }
        m_modules.append(moduleIndex);
        m_sinks.insert(index, sinkName);
    }

    m_active = true;
    return true;
}

QString StreamAudio::streamSink(int instanceIndex) const
{
    return m_sinks.value(instanceIndex);
}

QString StreamAudio::defaultSink() const
{
    return m_defaultSink;
}

void StreamAudio::teardown()
{
    for (const uint moduleIndex : std::as_const(m_modules)) {
        runPactl({QStringLiteral("unload-module"), QString::number(moduleIndex)}, nullptr);
    }

    if (!m_defaultSink.isEmpty() && (!m_modules.isEmpty() || m_active)) {
        QString current;
        if (!runPactl({QStringLiteral("get-default-sink")}, &current) || current != m_defaultSink) {
            runPactl({QStringLiteral("set-default-sink"), m_defaultSink}, nullptr);
        }
    }

    m_modules.clear();
    m_sinks.clear();
    m_active = false;
}

bool StreamAudio::isActive() const
{
    return m_active;
}

void StreamAudio::setProgramOverride(const QString &program)
{
    m_programOverride = program;
}
