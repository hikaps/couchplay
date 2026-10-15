// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "StreamManager.h"

#include "SunshineConfig.h"
#include "UserLookup.h"
#include "../dbus/CouchPlayHelperClient.h"

#include <QDebug>
#include <QDir>
#include <QSet>
#include <QTimer>

#include <pwd.h>
#include <unistd.h>

static const QString s_sunshineBinary = QStringLiteral("sunshine");
static constexpr int RESTART_DELAY_MS = 2000;

// The compositor for a streaming instance is the per-instance virtual gamescope
// Streaming display ownership is represented by the helper-issued display context.








StreamManager::StreamManager(QObject *parent)
    : QObject(parent)
{
}

void StreamManager::setHelperClient(CouchPlayHelperClient *helper)
{
    if (m_helperClient == helper) {
        return;
    }
    if (m_helperClient) {
        disconnect(m_helperClient, &CouchPlayHelperClient::instanceReady, this,
                   &StreamManager::onHelperInstanceReady);
        disconnect(m_helperClient, &CouchPlayHelperClient::instanceStopped, this,
                   &StreamManager::onHelperInstanceStopped);
    }
    m_helperClient = helper;
    if (m_helperClient) {
        connect(m_helperClient, &CouchPlayHelperClient::instanceReady, this,
                &StreamManager::onHelperInstanceReady);
        connect(m_helperClient, &CouchPlayHelperClient::instanceStopped, this,
                &StreamManager::onHelperInstanceStopped);
    }
}

StreamManager::~StreamManager()
{
    const QList<int> timers = m_startupTimers.keys();
    for (int idx : timers) {
        delete m_startupTimers.take(idx);
    }
    stopAll();
}

QVariantList StreamManager::streams() const
{
    QVariantList result;
    for (auto it = m_streams.constBegin(); it != m_streams.constEnd(); ++it) {
        QVariantMap entry;
        entry[QStringLiteral("instanceIndex")] = it.key();
        entry[QStringLiteral("state")] = it.value().state;
        entry[QStringLiteral("pid")] = it.value().pid;
        entry[QStringLiteral("configDir")] = it.value().configDir;
        result.append(entry);
    }
    return result;
}

bool StreamManager::autoRestart() const
{
    return m_autoRestart;
}

void StreamManager::setAutoRestart(bool enabled)
{
    if (m_autoRestart == enabled) {
        return;
    }
    m_autoRestart = enabled;
    Q_EMIT autoRestartChanged();
}

int StreamManager::startupTimeout() const
{
    return m_startupTimeout;
}

void StreamManager::setStartupTimeout(int timeoutMs)
{
    if (m_startupTimeout == timeoutMs) {
        return;
    }
    m_startupTimeout = timeoutMs;
    Q_EMIT startupTimeoutChanged();
}

bool StreamManager::startStream(int instanceIndex, const QVariantMap &config)
{
    if (instanceIndex < 0) {
        qWarning() << "StreamManager::startStream: invalid instanceIndex" << instanceIndex;
        return false;
    }

    if (m_streams.contains(instanceIndex)) {
        qWarning() << "StreamManager::startStream: instance" << instanceIndex << "already active";
        Q_EMIT streamError(instanceIndex, QStringLiteral("Instance already active"));
        return false;
    }

    const QString username = config.value(QStringLiteral("username")).toString();
    if (username.isEmpty()) {
        qWarning() << "StreamManager::startStream: no username for instance" << instanceIndex;
        Q_EMIT streamError(instanceIndex, QStringLiteral("No username configured"));
        return false;
    }

    const QString configDir = SunshineConfig::defaultConfigDir(instanceIndex);
    const QString configPath = SunshineConfig::generateConfig(config, instanceIndex, configDir);
    if (configPath.isEmpty()) {
        qWarning() << "StreamManager::startStream: failed to generate config for instance" << instanceIndex;
        Q_EMIT streamError(instanceIndex, QStringLiteral("Failed to generate Sunshine config"));
        return false;
    }

    StreamEntry entry;
    entry.instanceIndex = instanceIndex;
    entry.configDir = configDir;
    entry.username = username;
    entry.displayContext = config.value(QStringLiteral("displayContext")).toString();
    entry.lastConfig = config;
    entry.restartAttempts = 0;
    entry.state = NotStarted;
    entry.launching = true;
    m_streams[instanceIndex] = entry;

    setStreamState(instanceIndex, Waiting);
    Q_EMIT streamsChanged();

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qWarning() << "StreamManager: helper service not available";
        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("CouchPlay Helper service is not available"));
        cleanupConfigDir(instanceIndex);
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        return false;
    }

    const QStringList gameCommand{ s_sunshineBinary, configPath };
    const QString displayContext = entry.displayContext;
    const QStringList gamescopeArgs;
    const QStringList envVars;

    const qint64 pid = m_helperClient->launchAuxiliaryInstance(username,
                                                                displayContext,
                                                                gamescopeArgs,
                                                                gameCommand,
                                                                QString(),
                                                                QStringList(),
                                                                envVars,
                                                                false);
    if (pid <= 0) {
        qWarning() << "StreamManager: LaunchAuxiliaryInstance failed";
        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("Failed to launch Sunshine"));
        cleanupConfigDir(instanceIndex);
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        return false;
    }

    StreamEntry &launchedEntry = m_streams[instanceIndex];
    launchedEntry.pid = pid;
    launchedEntry.launching = false;
    if (launchedEntry.pendingReadyPid == pid) {
        launchedEntry.pendingReadyPid = 0;
        launchedEntry.ready = true;
        startStartupTimer(instanceIndex);
    }

    Q_EMIT streamsChanged();

    return true;
}

bool StreamManager::stopStream(int instanceIndex)
{
    if (!m_streams.contains(instanceIndex)) {
        return false;
    }

    if (m_startupTimers.contains(instanceIndex)) {
        delete m_startupTimers.take(instanceIndex);
    }

    if (m_restartTimers.contains(instanceIndex)) {
        if (m_restartTimers[instanceIndex]) {
            m_restartTimers[instanceIndex]->stop();
            m_restartTimers[instanceIndex]->deleteLater();
        }
        m_restartTimers.remove(instanceIndex);
    }

    const qint64 pid = m_streams[instanceIndex].pid;
    if (pid <= 0) {
        m_stoppingStreams.remove(instanceIndex);
        cleanupConfigDir(instanceIndex);
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        Q_EMIT streamStopped(instanceIndex);
        return true;
    }

    m_stoppingStreams.insert(instanceIndex);
    bool stopped = m_helperClient && m_helperClient->stopInstance(pid);
    if (!stopped && m_helperClient) {
        qWarning() << "StreamManager: StopInstance failed for PID" << pid << ", trying KillInstance";
        stopped = m_helperClient->killInstance(pid);
    }
    if (!stopped) {
        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("Failed to stop Sunshine"));
        Q_EMIT streamsChanged();
        return false;
    }

    if (!m_streams.contains(instanceIndex) || m_streams[instanceIndex].pid != pid) {
        return true;
    }
    m_stoppingStreams.remove(instanceIndex);
    m_streams[instanceIndex].pid = 0;
    m_streams[instanceIndex].ready = false;
    m_streams[instanceIndex].launching = false;
    setStreamState(instanceIndex, NotStarted);
    cleanupConfigDir(instanceIndex);
    m_streams.remove(instanceIndex);

    Q_EMIT streamsChanged();
    Q_EMIT streamStopped(instanceIndex);
    return true;
}

void StreamManager::stopAll()
{
    const QList<int> indices = m_streams.keys();
    for (int idx : indices) {
        stopStream(idx);
    }
}

StreamManager::StreamState StreamManager::streamState(int instanceIndex) const
{
    auto it = m_streams.constFind(instanceIndex);
    if (it != m_streams.constEnd()) {
        return it.value().state;
    }
    return NotStarted;
}

bool StreamManager::isStreaming(int instanceIndex) const
{
    return streamState(instanceIndex) == Streaming;
}
bool StreamManager::hasOwnedPids() const
{
    for (auto it = m_streams.constBegin(); it != m_streams.constEnd(); ++it) {
        if (it.value().pid > 0) {
            return true;
        }
    }
    return false;
}

bool StreamManager::hasOwnedPid(int instanceIndex) const
{
    const auto it = m_streams.constFind(instanceIndex);
    return it != m_streams.constEnd() && it.value().pid > 0;
}

void StreamManager::onHelperInstanceReady(const QString &username, qint64 pid)
{
    for (auto it = m_streams.begin(); it != m_streams.end(); ++it) {
        StreamEntry &entry = it.value();
        if (entry.username != username) {
            continue;
        }
        if (entry.pid == pid && pid > 0) {
            if (entry.ready || m_stoppingStreams.contains(it.key())) {
                return;
            }
            entry.ready = true;
            entry.launching = false;
            startStartupTimer(it.key());
            Q_EMIT streamsChanged();
            return;
        }
        if (entry.launching && entry.pid == 0 && pid > 0) {
            entry.pendingReadyPid = pid;
            return;
        }
    }
}

void StreamManager::startStartupTimer(int instanceIndex)
{
    if (!m_streams.contains(instanceIndex) || m_startupTimers.contains(instanceIndex)) {
        return;
    }
    const StreamEntry &entry = m_streams[instanceIndex];
    if (!entry.ready || entry.pid <= 0 || entry.state != Waiting || m_stoppingStreams.contains(instanceIndex)) {
        return;
    }
    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &StreamManager::onStartupTimeout);
    m_startupTimers[instanceIndex] = timer;
    timer->start(m_startupTimeout);
}

void StreamManager::onHelperInstanceStopped(const QString &username, qint64 pid, const QString &reason)
{
    int foundIndex = -1;
    for (auto it = m_streams.begin(); it != m_streams.end(); ++it) {
        if (it.value().username == username && it.value().pid == pid && pid > 0) {
            foundIndex = it.key();
            break;
        }
    }
    if (foundIndex < 0) {
        return;
    }

    const bool stopping = m_stoppingStreams.remove(foundIndex);
    const bool crashedDuringStartup = !m_streams[foundIndex].ready || m_startupTimers.contains(foundIndex);
    if (m_startupTimers.contains(foundIndex)) {
        delete m_startupTimers.take(foundIndex);
    }

    StreamEntry &entry = m_streams[foundIndex];
    entry.pid = 0;
    entry.ready = false;
    entry.launching = false;
    entry.pendingReadyPid = 0;
    setStreamState(foundIndex, stopping ? NotStarted : Error);
    cleanupConfigDir(foundIndex);

    if (stopping) {
        m_streams.remove(foundIndex);
        Q_EMIT streamsChanged();
        Q_EMIT streamStopped(foundIndex);
        return;
    }

    const QString errorMsg = (reason == QStringLiteral("crashed"))
        ? QStringLiteral("Sunshine crashed")
        : (reason == QStringLiteral("failed"))
            ? QStringLiteral("Sunshine failed to start")
            : QStringLiteral("Sunshine exited unexpectedly");

    if (m_autoRestart && entry.restartAttempts < StreamEntry::MAX_RESTART_ATTEMPTS) {
        if (crashedDuringStartup) {
            const int currentPort = entry.lastConfig.value(
                QStringLiteral("sunshinePort"), SunshineConfig::calculatePort(foundIndex)).toInt();
            QSet<int> usedPorts;
            for (auto it = m_streams.constBegin(); it != m_streams.constEnd(); ++it) {
                if (it.key() != foundIndex) {
                    usedPorts.insert(it.value().lastConfig.value(
                        QStringLiteral("sunshinePort"), SunshineConfig::calculatePort(it.key())).toInt());
                }
            }
            int bumped = currentPort + SunshineConfig::PORT_SPACING;
            while (usedPorts.contains(bumped) && bumped < static_cast<int>(SunshineConfig::MAX_PORT)) {
                bumped += SunshineConfig::PORT_SPACING;
            }
            entry.lastConfig[QStringLiteral("sunshinePort")] = bumped;
        }

        QTimer *restartTimer = new QTimer(this);
        restartTimer->setSingleShot(true);
        connect(restartTimer, &QTimer::timeout, this, [this, foundIndex]() {
            m_restartTimers.remove(foundIndex);
            attemptRestart(foundIndex);
        });
        m_restartTimers[foundIndex] = restartTimer;
        restartTimer->start(RESTART_DELAY_MS);
        const QString restartMessage = QStringLiteral("%1 (auto-restarting attempt %2/%3)")
            .arg(errorMsg)
            .arg(entry.restartAttempts + 1)
            .arg(StreamEntry::MAX_RESTART_ATTEMPTS);
        Q_EMIT streamsChanged();
        Q_EMIT streamError(foundIndex, restartMessage);
        return;
    }

    const QString finalMessage = m_autoRestart
        ? QStringLiteral("%1 (max restart attempts reached)").arg(errorMsg)
        : errorMsg;
    Q_EMIT streamError(foundIndex, finalMessage);
    if (!m_streams.contains(foundIndex)) {
        return;
    }
    m_streams.remove(foundIndex);
    Q_EMIT streamsChanged();
    Q_EMIT streamStopped(foundIndex);
}

void StreamManager::onStartupTimeout()
{
    QTimer *timer = qobject_cast<QTimer *>(sender());
    if (!timer) {
        return;
    }

    int timedOutIndex = -1;
    for (auto it = m_startupTimers.constBegin(); it != m_startupTimers.constEnd(); ++it) {
        if (it.value() == timer) {
            timedOutIndex = it.key();
            break;
        }
    }

    m_startupTimers.remove(timedOutIndex);
    timer->deleteLater();

    if (timedOutIndex < 0 || !m_streams.contains(timedOutIndex)) {
        return;
    }

    if (m_streams[timedOutIndex].state != Waiting) {
        return;
    }

    // Startup grace period elapsed, process still alive → confirm streaming
    setStreamState(timedOutIndex, Streaming);
    Q_EMIT streamsChanged();
    Q_EMIT streamStarted(timedOutIndex);
}

void StreamManager::attemptRestart(int instanceIndex)
{
    if (!m_streams.contains(instanceIndex)) {
        return;
    }

    StreamEntry &entry = m_streams[instanceIndex];
    if (entry.lastConfig.isEmpty()) {
        qWarning() << "StreamManager: no last config for restart of instance" << instanceIndex;
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        Q_EMIT streamStopped(instanceIndex);
        return;
    }

    entry.restartAttempts++;
    qDebug() << "StreamManager: attempting restart" << entry.restartAttempts
             << "of" << StreamEntry::MAX_RESTART_ATTEMPTS
             << "for instance" << instanceIndex;

    cleanupConfigDir(instanceIndex);

    entry.ready = false;
    entry.launching = true;
    entry.pendingReadyPid = 0;
    setStreamState(instanceIndex, Waiting);
    Q_EMIT streamsChanged();

    const QString configDir = SunshineConfig::defaultConfigDir(instanceIndex);
    const QString configPath = SunshineConfig::generateConfig(entry.lastConfig, instanceIndex, configDir);
    if (configPath.isEmpty()) {
        qWarning() << "StreamManager: restart failed — config generation error for instance" << instanceIndex;
        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("Auto-restart failed: could not generate config"));
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        Q_EMIT streamStopped(instanceIndex);
        return;
    }

    entry.configDir = configDir;

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qWarning() << "StreamManager: restart failed — helper not available for instance" << instanceIndex;
        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("Auto-restart failed: helper service not available"));
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        Q_EMIT streamStopped(instanceIndex);
        return;
    }

    const QStringList gameCommand{ s_sunshineBinary, configPath };
    const QString displayContext = entry.displayContext;
    const QStringList gamescopeArgs;
    const QStringList envVars;

    const qint64 pid = m_helperClient->launchAuxiliaryInstance(entry.username,
                                                                displayContext,
                                                                gamescopeArgs,
                                                                gameCommand,
                                                                QString(),
                                                                QStringList(),
                                                                envVars,
                                                                false);
    if (pid <= 0) {
        const QString detail = QStringLiteral("helper launch failed");
        qWarning() << "StreamManager: restart LaunchAuxiliaryInstance failed for instance" << instanceIndex << ":" << detail;
        entry.launching = false;
        entry.pendingReadyPid = 0;
        setStreamState(instanceIndex, Error);

        if (entry.restartAttempts >= StreamEntry::MAX_RESTART_ATTEMPTS) {
            const QString message = QStringLiteral("Auto-restart failed after %1 attempts: %2")
                .arg(entry.restartAttempts).arg(detail);
            Q_EMIT streamError(instanceIndex, message);
            if (!m_streams.contains(instanceIndex)) {
                return;
            }
            m_streams.remove(instanceIndex);
            Q_EMIT streamsChanged();
            Q_EMIT streamStopped(instanceIndex);
            return;
        }

        QTimer *restartTimer = new QTimer(this);
        restartTimer->setSingleShot(true);
        connect(restartTimer, &QTimer::timeout, this, [this, instanceIndex]() {
            m_restartTimers.remove(instanceIndex);
            attemptRestart(instanceIndex);
        });
        m_restartTimers[instanceIndex] = restartTimer;
        restartTimer->start(RESTART_DELAY_MS);
        Q_EMIT streamsChanged();
        Q_EMIT streamError(instanceIndex, QStringLiteral("Auto-restart attempt %1 failed: %2")
            .arg(entry.restartAttempts).arg(detail));
        return;
    }

    if (!m_streams.contains(instanceIndex)) {
        return;
    }
    StreamEntry &launchedEntry = m_streams[instanceIndex];
    launchedEntry.pid = pid;
    launchedEntry.launching = false;
    setStreamState(instanceIndex, Waiting);
    if (launchedEntry.pendingReadyPid == pid) {
        launchedEntry.pendingReadyPid = 0;
        launchedEntry.ready = true;
        startStartupTimer(instanceIndex);
    }
    Q_EMIT streamsChanged();
}

void StreamManager::setStreamState(int instanceIndex, StreamState state)
{
    if (m_streams.contains(instanceIndex)) {
        m_streams[instanceIndex].state = state;
    }
}

void StreamManager::cleanupConfigDir(int instanceIndex)
{
    if (!m_streams.contains(instanceIndex)) {
        return;
    }

    const QString configDir = m_streams[instanceIndex].configDir;
    if (configDir.isEmpty()) {
        return;
    }

    QDir dir(configDir);
    if (dir.exists()) {
        if (!dir.removeRecursively()) {
            qWarning() << "StreamManager: failed to remove config dir" << configDir;
        }
    }
}


