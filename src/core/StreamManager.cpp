// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "StreamManager.h"

#include "SunshineConfig.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QSet>
#include <QTimer>

#include <unistd.h>
#include <pwd.h>

static const QString s_helperService = QStringLiteral("io.github.hikaps.CouchPlayHelper");
static const QString s_helperPath = QStringLiteral("/io/github/hikaps/CouchPlayHelper");
static const QString s_helperInterface = QStringLiteral("io.github.hikaps.CouchPlayHelper");
static const QString s_sunshineBinary = QStringLiteral("sunshine");
static constexpr int RESTART_DELAY_MS = 2000;

// The compositor for a streaming instance is the per-instance virtual gamescope
// output, which the helper runs as the streaming user -- so its Wayland socket
// lives in /run/user/<streamingUserUid>, NOT the GUI user's runtime dir. Resolve
// the streaming user's uid; fall back to the GUI uid only if the lookup fails.
static uid_t resolveCompositorUid(const QString &username)
{
    if (const passwd *pwd = getpwnam(username.toLocal8Bit().constData())) {
        return pwd->pw_uid;
    }
    return ::getuid();
}

StreamManager::StreamManager(QObject *parent)
    : QObject(parent)
{
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
    entry.lastConfig = config;
    entry.restartAttempts = 0;
    entry.state = NotStarted;
    m_streams[instanceIndex] = entry;

    setStreamState(instanceIndex, Waiting);
    Q_EMIT streamsChanged();

    QDBusInterface helper(s_helperService, s_helperPath, s_helperInterface,
                          QDBusConnection::systemBus());

    if (!helper.isValid()) {
        qWarning() << "StreamManager: helper service not available";
        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("CouchPlay Helper service is not available"));
        cleanupConfigDir(instanceIndex);
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        return false;
    }

    const QString gameCommand = s_sunshineBinary + QLatin1Char(' ') + configPath;
    const uid_t compositorUid = resolveCompositorUid(username);
    const QStringList gamescopeArgs;
    const QStringList envVars;
    const QStringList bindPaths;

    QDBusReply<qint64> reply = helper.call(
        QStringLiteral("LaunchInstance"),
        username,
        static_cast<uint>(compositorUid),
        gamescopeArgs,
        gameCommand,
        envVars,
        bindPaths
    );

    if (!reply.isValid()) {
        const QString errorMsg = reply.error().message();
        qWarning() << "StreamManager: LaunchInstance failed:" << errorMsg;

        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("Failed to launch Sunshine: %1").arg(errorMsg));
        cleanupConfigDir(instanceIndex);
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        return false;
    }

    const qint64 pid = reply.value();
    if (pid == 0) {
        qWarning() << "StreamManager: helper returned PID 0";
        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("Helper returned invalid PID"));
        cleanupConfigDir(instanceIndex);
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        return false;
    }

    m_streams[instanceIndex].pid = pid;
    setStreamState(instanceIndex, Waiting);

    QDBusConnection::systemBus().connect(
        s_helperService, s_helperPath, s_helperInterface,
        QStringLiteral("instanceStopped"),
        this, SLOT(onHelperInstanceStopped(QString, qint64, QString))
    );

    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &StreamManager::onStartupTimeout);
    m_startupTimers[instanceIndex] = timer;
    timer->start(m_startupTimeout);

    Q_EMIT streamsChanged();

    return true;
}

bool StreamManager::stopStream(int instanceIndex)
{
    if (!m_streams.contains(instanceIndex)) {
        return false;
    }

    StreamEntry &entry = m_streams[instanceIndex];

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

    if (entry.pid == 0) {
        cleanupConfigDir(instanceIndex);
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        return true;
    }

    QDBusInterface helper(s_helperService, s_helperPath, s_helperInterface,
                          QDBusConnection::systemBus());

    if (helper.isValid()) {
        QDBusReply<bool> reply = helper.call(QStringLiteral("StopInstance"), entry.pid);
        if (!reply.isValid() || !reply.value()) {
            qWarning() << "StreamManager: StopInstance failed for PID" << entry.pid << ", trying KillInstance";
            helper.call(QStringLiteral("KillInstance"), entry.pid);
        }
    }

    entry.pid = 0;
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

void StreamManager::onHelperInstanceStopped(const QString &username, qint64 pid, const QString &reason)
{
    Q_UNUSED(username)

    int foundIndex = -1;
    for (auto it = m_streams.begin(); it != m_streams.end(); ++it) {
        if (it.value().pid == pid) {
            foundIndex = it.key();
            break;
        }
    }

    if (foundIndex < 0) {
        return;
    }

    bool crashedDuringStartup = m_startupTimers.contains(foundIndex);
    if (crashedDuringStartup) {
        delete m_startupTimers.take(foundIndex);
    }

    const QString errorMsg = (reason == QStringLiteral("crashed"))
        ? QStringLiteral("Sunshine crashed")
        : (reason == QStringLiteral("failed"))
            ? QStringLiteral("Sunshine failed to start")
            : QStringLiteral("Sunshine exited unexpectedly");

    m_streams[foundIndex].pid = 0;
    setStreamState(foundIndex, Error);
    cleanupConfigDir(foundIndex);

    if (m_autoRestart && m_streams[foundIndex].restartAttempts < StreamEntry::MAX_RESTART_ATTEMPTS) {
        if (crashedDuringStartup) {
            // Retry on a port no sibling stream is using -- each Sunshine instance
            // owns a PORT_SPACING-wide range, so reusing a sibling's base port would
            // evict it. (Bumping by exactly PORT_SPACING would otherwise land on the
            // next instance's default slot: calculatePort(n+1) == calculatePort(n)+30.)
            const int currentPort = m_streams[foundIndex].lastConfig.value(
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
            m_streams[foundIndex].lastConfig[QStringLiteral("sunshinePort")] = bumped;
        }
        Q_EMIT streamError(foundIndex, QStringLiteral("%1 (auto-restarting attempt %2/%3)")
            .arg(errorMsg)
            .arg(m_streams[foundIndex].restartAttempts + 1)
            .arg(StreamEntry::MAX_RESTART_ATTEMPTS));
        Q_EMIT streamsChanged();

        QTimer *restartTimer = new QTimer(this);
        restartTimer->setSingleShot(true);
        connect(restartTimer, &QTimer::timeout, this, [this, foundIndex]() {
            m_restartTimers.remove(foundIndex);
            attemptRestart(foundIndex);
        });
        m_restartTimers[foundIndex] = restartTimer;
        restartTimer->start(RESTART_DELAY_MS);
    } else {
        if (m_autoRestart) {
            Q_EMIT streamError(foundIndex, QStringLiteral("%1 (max restart attempts reached)").arg(errorMsg));
        } else {
            Q_EMIT streamError(foundIndex, errorMsg);
        }
        m_streams.remove(foundIndex);
        Q_EMIT streamsChanged();
        Q_EMIT streamStopped(foundIndex);
    }
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

    QDBusInterface helper(s_helperService, s_helperPath, s_helperInterface,
                          QDBusConnection::systemBus());

    if (!helper.isValid()) {
        qWarning() << "StreamManager: restart failed — helper not available for instance" << instanceIndex;
        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("Auto-restart failed: helper service not available"));
        m_streams.remove(instanceIndex);
        Q_EMIT streamsChanged();
        Q_EMIT streamStopped(instanceIndex);
        return;
    }

    const QString gameCommand = s_sunshineBinary + QLatin1Char(' ') + configPath;
    const uid_t compositorUid = resolveCompositorUid(entry.username);
    const QStringList gamescopeArgs;
    const QStringList envVars;
    const QStringList bindPaths;

    QDBusReply<qint64> reply = helper.call(
        QStringLiteral("LaunchInstance"),
        entry.username,
        static_cast<uint>(compositorUid),
        gamescopeArgs,
        gameCommand,
        envVars,
        bindPaths
    );

    if (!reply.isValid() || reply.value() <= 0) {
        const QString detail = reply.isValid()
            ? QStringLiteral("invalid PID returned")
            : reply.error().message();
        qWarning() << "StreamManager: restart LaunchInstance failed for instance" << instanceIndex << ":" << detail;

        if (entry.restartAttempts >= StreamEntry::MAX_RESTART_ATTEMPTS) {
            setStreamState(instanceIndex, Error);
            Q_EMIT streamError(instanceIndex, QStringLiteral("Auto-restart failed after %1 attempts: %2")
                .arg(entry.restartAttempts).arg(detail));
            m_streams.remove(instanceIndex);
            Q_EMIT streamsChanged();
            Q_EMIT streamStopped(instanceIndex);
            return;
        }

        setStreamState(instanceIndex, Error);
        Q_EMIT streamError(instanceIndex, QStringLiteral("Auto-restart attempt %1 failed: %2")
            .arg(entry.restartAttempts).arg(detail));

        QTimer *restartTimer = new QTimer(this);
        restartTimer->setSingleShot(true);
        connect(restartTimer, &QTimer::timeout, this, [this, instanceIndex]() {
            m_restartTimers.remove(instanceIndex);
            attemptRestart(instanceIndex);
        });
        m_restartTimers[instanceIndex] = restartTimer;
        restartTimer->start(RESTART_DELAY_MS);
        return;
    }

    entry.pid = reply.value();
    setStreamState(instanceIndex, Waiting);

    QDBusConnection::systemBus().connect(
        s_helperService, s_helperPath, s_helperInterface,
        QStringLiteral("instanceStopped"),
        this, SLOT(onHelperInstanceStopped(QString, qint64, QString))
    );

    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &StreamManager::onStartupTimeout);
    m_startupTimers[instanceIndex] = timer;
    timer->start(m_startupTimeout);

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


