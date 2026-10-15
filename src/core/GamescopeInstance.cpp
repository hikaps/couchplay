// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "GamescopeInstance.h"
#include "PresetManager.h"

#include "../dbus/CouchPlayHelperClient.h"

#include <QProcess>
#include <QDir>
#include <QFile>

#include <pwd.h>
#include <unistd.h>

GamescopeInstance::GamescopeInstance(QObject *parent)
    : QObject(parent)
{
}

GamescopeInstance::~GamescopeInstance()
{
    stop();
}

void GamescopeInstance::setVirtualDisplaySocket(const QString &socket)
{
    if (m_virtualDisplaySocket != socket) {
        m_virtualDisplaySocket = socket;
        Q_EMIT configChanged();
    }
}

void GamescopeInstance::setHelperClient(CouchPlayHelperClient *client)
{
    if (m_helperClient == client) {
        return;
    }
    if (m_helperClient) {
        disconnect(m_helperClient, &CouchPlayHelperClient::instanceReady, this,
                   &GamescopeInstance::onHelperInstanceReady);
        disconnect(m_helperClient, &CouchPlayHelperClient::instanceStopped, this,
                   &GamescopeInstance::onHelperInstanceStopped);
    }
    m_helperClient = client;
    if (m_helperClient) {
        connect(m_helperClient, &CouchPlayHelperClient::instanceReady, this,
                &GamescopeInstance::onHelperInstanceReady);
        connect(m_helperClient, &CouchPlayHelperClient::instanceStopped, this,
                &GamescopeInstance::onHelperInstanceStopped);
    }
}

bool GamescopeInstance::start(const QVariantMap &config, int index, const QVariantList &directoryPreparations)
{
    if (m_helperPid > 0) {
        Q_EMIT errorOccurred(QStringLiteral("Instance already running"));
        return false;
    }

    m_index = index;
    m_username = config.value(QStringLiteral("username")).toString();
    const int posX = config.value(QStringLiteral("positionX"), 0).toInt();
    const int posY = config.value(QStringLiteral("positionY"), 0).toInt();
    const int outputW = config.value(QStringLiteral("outputWidth"), 960).toInt();
    const int outputH = config.value(QStringLiteral("outputHeight"), 1080).toInt();
    m_windowGeometry = QRect(posX, posY, outputW, outputH);
    m_virtualDisplaySocket = config.value(QStringLiteral("displayContext")).toString();
    Q_EMIT configChanged();

    if (!m_helperClient || !m_helperClient->isAvailable()) {
        qWarning() << "Instance" << m_index << "helper service not available";
        Q_EMIT errorOccurred(QStringLiteral(
            "CouchPlay Helper service is not available. Please run: sudo ./scripts/install-helper.sh install"));
        return false;
    }

    const QStringList gamescopeArgs = buildGamescopeArgs(config);
    const QStringList envVars = buildEnvironment(config);
    const QStringList gameCommand = config.value(QStringLiteral("gameCommand")).toStringList();
    if (gameCommand.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Launch command is empty"));
        return false;
    }

    const QString displayContext = config.value(QStringLiteral("displayContext")).toString();
    const QString workingDirectory = config.value(QStringLiteral("workingDirectory")).toString();
    const QStringList sharedRoots = config.value(QStringLiteral("sharedRoots")).toStringList();
    m_pendingReadyUsername.clear();
    m_pendingReadyPid = 0;
    m_pendingStoppedUsername.clear();
    m_pendingStoppedPid = 0;
    m_pendingStopReason.clear();
    QString helperError;
    const QMetaObject::Connection helperErrorConnection = connect(
        m_helperClient, &CouchPlayHelperClient::errorOccurred, this, [&helperError](const QString &message) {
            helperError = message;
        });
    m_launchRequestPending = true;
    const qint64 pid = m_helperClient->launchInstance(m_username,
                                                       displayContext,
                                                       gamescopeArgs,
                                                       gameCommand,
                                                       workingDirectory,
                                                       sharedRoots,
                                                       envVars,
                                                       config.value(QStringLiteral("privateTemporaryDirectories")).toBool(),
                                                       directoryPreparations);
    m_launchRequestPending = false;
    disconnect(helperErrorConnection);
    if (pid <= 0) {
        m_pendingReadyUsername.clear();
        m_pendingReadyPid = 0;
        m_pendingStoppedUsername.clear();
        m_pendingStoppedPid = 0;
        m_pendingStopReason.clear();
        qWarning() << "Instance" << m_index << "helper LaunchInstance failed";
        Q_EMIT errorOccurred(helperError.isEmpty()
                                  ? QStringLiteral("Failed to launch instance through CouchPlay Helper")
                                  : helperError);
        return false;
    }

    m_helperPid = pid;
    if (m_pendingStoppedUsername == m_username && m_pendingStoppedPid == pid) {
        const QString reason = m_pendingStopReason;
        onHelperInstanceStopped(m_username, pid, reason);
    } else if (m_pendingReadyUsername == m_username && m_pendingReadyPid == pid) {
        onHelperInstanceReady(m_username, pid);
    } else {
        setStatus(QStringLiteral("Preparing data"));
        Q_EMIT runningChanged();
    }
    return true;
}

void GamescopeInstance::stop(int timeoutMs)
{
    Q_UNUSED(timeoutMs)
    const qint64 pid = m_helperPid;
    if (pid <= 0) return;
    setStatus(QStringLiteral("Stopping..."));
    bool didStop = m_helperClient && m_helperClient->stopInstance(pid);
    if (!didStop && m_helperClient) didStop = m_helperClient->killInstance(pid);
    if (!didStop) {
        setStatus(QStringLiteral("Stop failed"));
        Q_EMIT errorOccurred(QStringLiteral("Failed to stop instance %1").arg(m_username));
        return;
    }
    if (m_helperPid != pid) return;
    m_helperPid = 0;
    m_gamescopePid = 0;
    m_pendingReadyUsername.clear();
    m_pendingReadyPid = 0;
    m_pendingStoppedUsername.clear();
    m_pendingStoppedPid = 0;
    m_pendingStopReason.clear();
    setStatus(QStringLiteral("Stopped"));
    Q_EMIT gamescopePidChanged();
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

void GamescopeInstance::kill()
{
    const qint64 pid = m_helperPid;
    if (pid <= 0) return;
    setStatus(QStringLiteral("Killing..."));
    if (!m_helperClient || !m_helperClient->killInstance(pid)) {
        setStatus(QStringLiteral("Kill failed"));
        Q_EMIT errorOccurred(QStringLiteral("Failed to kill instance %1").arg(m_username));
        return;
    }
    if (m_helperPid != pid) return;
    m_helperPid = 0;
    m_gamescopePid = 0;
    m_pendingReadyUsername.clear();
    m_pendingReadyPid = 0;
    m_pendingStoppedUsername.clear();
    m_pendingStoppedPid = 0;
    m_pendingStopReason.clear();
    setStatus(QStringLiteral("Killed"));
    Q_EMIT gamescopePidChanged();
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

bool GamescopeInstance::isRunning() const
{
    return m_gamescopePid > 0;
}

void GamescopeInstance::setStatus(const QString &status)
{
    if (m_status != status) {
        m_status = status;
        Q_EMIT statusChanged();
    }
}

QStringList GamescopeInstance::buildGamescopeArgs(const QVariantMap &config)
{
    QStringList args;

    if (config.value(QStringLiteral("launcherId")).toString() == QStringLiteral("steam")) {
        args << QStringLiteral("-e");
    }

    bool borderless = config.value(QStringLiteral("borderless"), false).toBool();
    if (borderless) {
        args << QStringLiteral("-b");
    }

    // Note: Don't pass --backend flag - let gamescope auto-detect
    // It will use wayland backend when WAYLAND_DISPLAY is set

    int internalW = config.value(QStringLiteral("internalWidth"), 1920).toInt();
    int internalH = config.value(QStringLiteral("internalHeight"), 1080).toInt();
    args << QStringLiteral("-w") << QString::number(internalW);
    args << QStringLiteral("-h") << QString::number(internalH);

    QString outputMode = config.value(QStringLiteral("outputMode")).toString();
    bool isStreaming = (outputMode == QStringLiteral("streaming"));

    int outputW, outputH;
    if (isStreaming) {
        QString streamRes = config.value(QStringLiteral("streamResolution"), QStringLiteral("1920x1080")).toString();
        QStringList parts = streamRes.split(QLatin1Char('x'));
        if (parts.size() == 2) {
            outputW = parts[0].toInt();
            outputH = parts[1].toInt();
        } else {
            outputW = 1920;
            outputH = 1080;
        }
    } else {
        outputW = config.value(QStringLiteral("outputWidth"), 960).toInt();
        outputH = config.value(QStringLiteral("outputHeight"), 1080).toInt();
    }
    args << QStringLiteral("-W") << QString::number(outputW);
    args << QStringLiteral("-H") << QString::number(outputH);

    int refreshRate = config.value(QStringLiteral("refreshRate"), 60).toInt();
    if (refreshRate > 0) {
        args << QStringLiteral("-r") << QString::number(refreshRate);
    }

    // Scaling mode: auto, integer, fit, fill, stretch
    QString scalingMode = config.value(QStringLiteral("scalingMode"), QStringLiteral("fit")).toString();
    if (!scalingMode.isEmpty() && scalingMode != QStringLiteral("auto")) {
        args << QStringLiteral("-S") << scalingMode;
    }

    // Filter mode: linear, nearest, fsr, nis
    QString filterMode = config.value(QStringLiteral("filterMode"), QStringLiteral("linear")).toString();
    if (!filterMode.isEmpty()) {
        args << QStringLiteral("-F") << filterMode;
    }

    // Window positioning is handled by WindowManager via KWin scripting
    // (gamescope does not have a --position flag)

    if (!isStreaming) {
        QString monitorName = config.value(QStringLiteral("monitorName")).toString();
        if (!monitorName.isEmpty()) {
            args << QStringLiteral("--prefer-output") << monitorName;
        }
    }

    // NOTE: Input device isolation is handled via device ownership (chown/chmod),
    // NOT via gamescope flags. The --input-device flag doesn't exist in gamescope,
    // and --grab only grabs the keyboard, not gamepads/controllers.
    // Device isolation works because each user can only read devices they own.
    // See helper's ChangeDeviceOwner() method.

    return args;
}

QStringList GamescopeInstance::buildEnvironment(const QVariantMap &config)
{
    QStringList envVars;

    // Enable Gamescope WSI layer - critical for Vulkan games to work inside gamescope
    envVars << QStringLiteral("ENABLE_GAMESCOPE_WSI=1");

    // Prevent games from minimizing when losing focus
    envVars << QStringLiteral("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0");

    // Mesa threading for better performance
    envVars << QStringLiteral("mesa_glthread=true");

    // Set desktop environment for XDG portal integration (native file dialogs in Steam etc.)
    envVars << QStringLiteral("XDG_CURRENT_DESKTOP=KDE");
    envVars << QStringLiteral("GTK_USE_PORTAL=1");

    // Route game audio to the per-instance null sink for Sunshine capture
    QString sinkName = config.value(QStringLiteral("sink")).toString();
    if (!sinkName.isEmpty()) {
        envVars << QStringLiteral("PULSE_SINK=%1").arg(sinkName);
    }

    return envVars;
}

void GamescopeInstance::onHelperInstanceReady(const QString &username, qint64 pid)
{
    if (username != m_username || pid <= 0 || (m_pendingStoppedUsername == username && m_pendingStoppedPid == pid)) {
        return;
    }
    if (m_helperPid <= 0) {
        if (m_launchRequestPending) {
            m_pendingReadyUsername = username;
            m_pendingReadyPid = pid;
        }
        return;
    }
    if (pid != m_helperPid || m_gamescopePid == pid) return;
    m_pendingReadyUsername.clear();
    m_pendingReadyPid = 0;
    m_gamescopePid = pid;
    setStatus(QStringLiteral("Running as %1").arg(m_username));
    Q_EMIT gamescopePidChanged();
    Q_EMIT runningChanged();
    Q_EMIT started();
}

void GamescopeInstance::onHelperInstanceStopped(const QString &username, qint64 pid, const QString &reason)
{
    if (username != m_username || pid <= 0) return;
    if (m_helperPid <= 0) {
        if (m_launchRequestPending) {
            m_pendingStoppedUsername = username;
            m_pendingStoppedPid = pid;
            m_pendingStopReason = reason;
        }
        return;
    }
    if (pid != m_helperPid) return;
    const QString statusMsg = reason == QStringLiteral("crashed")
        ? QStringLiteral("Crashed")
        : reason == QStringLiteral("failed") ? QStringLiteral("Failed")
                                               : QStringLiteral("Exited unexpectedly");
    m_helperPid = 0;
    m_gamescopePid = 0;
    m_pendingReadyUsername.clear();
    m_pendingReadyPid = 0;
    m_pendingStoppedUsername.clear();
    m_pendingStoppedPid = 0;
    m_pendingStopReason.clear();
    setStatus(statusMsg);
    Q_EMIT gamescopePidChanged();
    Q_EMIT runningChanged();
    Q_EMIT errorOccurred(reason.isEmpty() ? statusMsg : reason);
    Q_EMIT stopped();
}
