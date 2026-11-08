// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "VirtualDisplayManager.h"

#include "HostProcess.h"

#include <QGuiApplication>
#include <QProcess>
#include <QScreen>

namespace
{
const QString kVdisplayBinary = QStringLiteral("couchplay-vdisplay");

QSet<QString> currentScreenNames()
{
    QSet<QString> names;
    const auto screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        names.insert(screen->name());
    }
    return names;
}

bool lookupQtScreen(const QString &expectedName,
                    const QSize &size,
                    const QSet<QString> &knownBefore,
                    QString *name,
                    QRect *geometry)
{
    const auto screens = QGuiApplication::screens();
    QScreen *match = nullptr;
    for (QScreen *screen : screens) {
        if (screen->name() == expectedName) {
            match = screen;
            break;
        }
    }
    if (!match) {
        // KWin may name the output differently; accept a new screen of the requested size.
        for (QScreen *screen : screens) {
            if (!knownBefore.contains(screen->name()) && screen->geometry().size() == size) {
                match = screen;
                break;
            }
        }
    }
    if (!match) {
        return false;
    }
    *name = match->name();
    *geometry = match->geometry();
    return true;
}
}

VirtualDisplayManager::VirtualDisplayManager(QObject *parent)
    : QObject(parent)
    , m_screenLookup(lookupQtScreen)
{
}

VirtualDisplayManager::~VirtualDisplayManager()
{
    destroyAll();
}

QString VirtualDisplayManager::expectedOutputName(int instanceIndex)
{
    return QStringLiteral("Virtual-couchplay-p%1").arg(instanceIndex + 1);
}

QString VirtualDisplayManager::outputName(int instanceIndex) const
{
    const auto it = m_displays.constFind(instanceIndex);
    return it == m_displays.cend() ? QString() : it->outputName;
}

void VirtualDisplayManager::create(int instanceIndex, const QSize &size)
{
    if (m_displays.contains(instanceIndex)) {
        Q_EMIT failed(instanceIndex, QStringLiteral("Virtual display already exists"));
        return;
    }

    const QString program = m_programOverride.isEmpty() ? HostProcess::hostLibexecPath(kVdisplayBinary)
                                                        : m_programOverride;
    if (program.isEmpty()) {
        Q_EMIT failed(instanceIndex,
                      QStringLiteral("couchplay-vdisplay is not installed; reinstall the CouchPlay helper"));
        return;
    }

    Display display;
    display.size = size;
    display.knownBefore = currentScreenNames();
    display.process = new QProcess(this);
    display.process->setProcessChannelMode(QProcess::SeparateChannels);
    const QStringList arguments{QStringLiteral("--name"),
                                QStringLiteral("couchplay-p%1").arg(instanceIndex + 1),
                                QStringLiteral("--width"),
                                QString::number(size.width()),
                                QStringLiteral("--height"),
                                QString::number(size.height())};
    HostProcess::configure(*display.process, program, arguments);

    connect(display.process, &QProcess::readyReadStandardOutput, this, [this, instanceIndex]() {
        onStdout(instanceIndex);
    });
    connect(display.process, &QProcess::readyReadStandardError, this, [this, instanceIndex]() {
        auto it = m_displays.find(instanceIndex);
        if (it != m_displays.end()) {
            it->stderrText += QString::fromUtf8(it->process->readAllStandardError());
        }
    });
    connect(display.process, &QProcess::errorOccurred, this, [this, instanceIndex](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            fail(instanceIndex, QStringLiteral("Could not start couchplay-vdisplay"));
        }
    });
    connect(display.process,
            &QProcess::finished,
            this,
            [this, instanceIndex](int exitCode, QProcess::ExitStatus status) {
                auto it = m_displays.find(instanceIndex);
                if (it == m_displays.end() || it->finished) {
                    return;
                }
                it->stderrText += QString::fromUtf8(it->process->readAllStandardError());
                QString reason = it->stderrText.trimmed();
                if (reason.isEmpty()) {
                    reason = status == QProcess::NormalExit
                        ? QStringLiteral("couchplay-vdisplay exited with code %1").arg(exitCode)
                        : QStringLiteral("couchplay-vdisplay crashed");
                }
                fail(instanceIndex, reason);
            });

    display.pollTimer = new QTimer(this);
    display.pollTimer->setInterval(100);
    connect(display.pollTimer, &QTimer::timeout, this, [this, instanceIndex]() { poll(instanceIndex); });

    display.timeoutTimer = new QTimer(this);
    display.timeoutTimer->setSingleShot(true);
    display.timeoutTimer->setInterval(m_timeoutMs);
    connect(display.timeoutTimer, &QTimer::timeout, this, [this, instanceIndex]() {
        fail(instanceIndex, QStringLiteral("Timed out waiting for the virtual display"));
    });

    QProcess *process = display.process;
    display.timeoutTimer->start();
    m_displays.insert(instanceIndex, display);
    process->start();
}

void VirtualDisplayManager::onStdout(int index)
{
    auto it = m_displays.find(index);
    if (it == m_displays.end()) {
        return;
    }
    it->buffer += it->process->readAllStandardOutput();
    while (true) {
        const qsizetype newline = it->buffer.indexOf('\n');
        if (newline < 0) {
            break;
        }
        const QByteArray line = it->buffer.left(newline).trimmed();
        it->buffer.remove(0, newline + 1);
        if (line.startsWith("READY") && !it->nodeReported) {
            it->nodeReported = true;
            it->pollTimer->start();
            poll(index);
            return; // poll may have erased the entry
        }
    }
}

void VirtualDisplayManager::poll(int index)
{
    auto it = m_displays.find(index);
    if (it == m_displays.end() || it->finished || !it->nodeReported) {
        return;
    }
    QString name;
    QRect geometry;
    if (!m_screenLookup(expectedOutputName(index), it->size, it->knownBefore, &name, &geometry)) {
        return;
    }
    it->outputName = name;
    finish(*it);
    Q_EMIT ready(index, name, geometry);
}

void VirtualDisplayManager::finish(Display &display)
{
    display.finished = true;
    display.pollTimer->stop();
    display.timeoutTimer->stop();
}

void VirtualDisplayManager::fail(int index, const QString &message)
{
    auto it = m_displays.find(index);
    if (it == m_displays.end() || it->finished) {
        return;
    }
    finish(*it);
    // Drop the process that failed so destroyAll() has nothing left to wait for.
    QProcess *process = it->process;
    QTimer *pollTimer = it->pollTimer;
    QTimer *timeoutTimer = it->timeoutTimer;
    m_displays.erase(it);
    pollTimer->deleteLater();
    timeoutTimer->deleteLater();
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(1000)) {
            process->kill();
            process->waitForFinished(500);
        }
    }
    process->deleteLater();
    Q_EMIT failed(index, message);
}

void VirtualDisplayManager::destroyAll()
{
    const auto displays = std::exchange(m_displays, {});
    for (const Display &display : displays) {
        display.pollTimer->stop();
        display.timeoutTimer->stop();
        display.pollTimer->deleteLater();
        display.timeoutTimer->deleteLater();
        display.process->disconnect(this);
        if (display.process->state() != QProcess::NotRunning) {
            display.process->terminate();
            if (!display.process->waitForFinished(3000)) {
                display.process->kill();
                display.process->waitForFinished(1000);
            }
        }
        display.process->deleteLater();
    }
}
