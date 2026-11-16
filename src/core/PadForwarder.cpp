// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "PadForwarder.h"

#include "HostProcess.h"

#include <QDeadlineTimer>
#include <QDebug>

namespace
{
QString &programOverride()
{
    static QString program;
    return program;
}
}

PadForwarder::PadForwarder(QObject *parent)
    : QObject(parent)
{
    m_process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &PadForwarder::drainOutput);
}

PadForwarder::~PadForwarder()
{
    stop();
}

void PadForwarder::setProgramOverrideForTesting(const QString &program)
{
    programOverride() = program;
}

bool PadForwarder::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

bool PadForwarder::start(int playerIndex, int timeoutMs)
{
    const QString program = programOverride().isEmpty() ? HostProcess::hostLibexecPath(QStringLiteral("couchplay-padfwd"))
                                                        : programOverride();
    if (program.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("couchplay-padfwd was not found. Reinstall CouchPlay."));
        return false;
    }

    m_node.clear();
    m_pending.clear();
    m_startError.clear();

    const QString name = QStringLiteral("CouchPlay P%1 pad").arg(playerIndex + 1);
    HostProcess::configure(m_process, program, {QStringLiteral("--name"), name});
    m_process.start();
    if (!m_process.waitForStarted(timeoutMs)) {
        Q_EMIT errorOccurred(QStringLiteral("Could not start couchplay-padfwd: %1").arg(m_process.errorString()));
        return false;
    }

    QDeadlineTimer deadline(timeoutMs);
    while (m_node.isEmpty() && m_startError.isEmpty() && !deadline.hasExpired()) {
        if (m_process.state() == QProcess::NotRunning) {
            drainOutput();
            break;
        }
        if (!m_process.waitForReadyRead(static_cast<int>(qMax<qint64>(1, deadline.remainingTime())))) {
            break;
        }
    }

    if (m_node.isEmpty()) {
        const QString reason = m_startError.isEmpty() ? QStringLiteral("timed out waiting for the virtual pad")
                                                       : m_startError;
        stop();
        Q_EMIT errorOccurred(QStringLiteral("Virtual pad failed: %1. /dev/uinput must be writable by your user.")
                                 .arg(reason));
        return false;
    }
    return true;
}

void PadForwarder::drainOutput()
{
    m_pending += QString::fromUtf8(m_process.readAllStandardOutput());
    qsizetype newline;
    while ((newline = m_pending.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_pending.left(newline).trimmed();
        m_pending.remove(0, newline + 1);
        if (!line.isEmpty()) {
            handleLine(line);
        }
    }
}

void PadForwarder::handleLine(const QString &line)
{
    if (line.startsWith(QStringLiteral("ready "))) {
        m_node = line.mid(6).trimmed();
    } else if (line.startsWith(QStringLiteral("lost "))) {
        Q_EMIT sourceLost(line.mid(5).trimmed());
    } else if (line == QStringLiteral("chord")) {
        Q_EMIT chordTriggered();
    } else if (line.startsWith(QStringLiteral("error "))) {
        const QString message = line.mid(6).trimmed();
        if (m_node.isEmpty()) {
            m_startError = message;
        } else {
            qWarning() << "couchplay-padfwd:" << message;
        }
    }
}

void PadForwarder::sendLine(const QString &line)
{
    if (m_process.state() == QProcess::Running) {
        m_process.write(line.toUtf8() + '\n');
    }
}

void PadForwarder::attach(const QString &devicePath)
{
    sendLine(QStringLiteral("attach ") + devicePath);
}

void PadForwarder::detach(const QString &devicePath)
{
    sendLine(QStringLiteral("detach ") + devicePath);
}

void PadForwarder::stop()
{
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    m_process.closeWriteChannel();
    if (!m_process.waitForFinished(2000)) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
    m_node.clear();
}
