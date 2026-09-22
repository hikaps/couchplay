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
        disconnect(m_helperClient, &CouchPlayHelperClient::instanceStopped, this,
                   &GamescopeInstance::onHelperInstanceStopped);
    }
    m_helperClient = client;
    if (m_helperClient) {
        connect(m_helperClient, &CouchPlayHelperClient::instanceStopped, this,
                &GamescopeInstance::onHelperInstanceStopped);
    }
}

bool GamescopeInstance::start(const QVariantMap &config, int index)
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
    QStringList gameCommand = config.value(QStringLiteral("gameCommand")).toStringList();
    if (gameCommand.isEmpty()) {
        gameCommand = QProcess::splitCommand(config.value(QStringLiteral("presetCommand")).toString());
    }
    if (gameCommand.isEmpty()) {
        gameCommand = QProcess::splitCommand(PresetManager::defaultSteamCommand());
    }

    const QString displayContext = config.value(QStringLiteral("displayContext")).toString();
    const QString workingDirectory = config.value(QStringLiteral("workingDirectory")).toString();
    const QStringList bindPaths = config.value(QStringLiteral("overrideBinds")).toStringList();
    const QStringList sharedRoots = config.value(QStringLiteral("sharedRoots")).toStringList();
    QString helperError;
    const QMetaObject::Connection helperErrorConnection = connect(
        m_helperClient, &CouchPlayHelperClient::errorOccurred, this, [&helperError](const QString &message) {
            helperError = message;
        });
    const qint64 pid = m_helperClient->launchInstance(m_username,
                                                       displayContext,
                                                       gamescopeArgs,
                                                       gameCommand,
                                                       workingDirectory,
                                                       sharedRoots,
                                                       envVars,
                                                       bindPaths);
    disconnect(helperErrorConnection);
    if (pid <= 0) {
        qWarning() << "Instance" << m_index << "helper LaunchInstance failed";
        Q_EMIT errorOccurred(helperError.isEmpty()
                                  ? QStringLiteral("Failed to launch instance through CouchPlay Helper")
                                  : helperError);
        return false;
    }

    m_helperPid = pid;
    m_gamescopePid = pid;
    Q_EMIT gamescopePidChanged();
    setStatus(QStringLiteral("Running as %1").arg(m_username));
    Q_EMIT runningChanged();
    Q_EMIT started();
    return true;
}

void GamescopeInstance::stop(int timeoutMs)
{
    Q_UNUSED(timeoutMs)

    if (m_helperPid <= 0) {
        return;
    }

    setStatus(QStringLiteral("Stopping..."));
    if (m_helperClient && (!m_helperClient->stopInstance(m_helperPid))) {
        qWarning() << "Instance" << m_index << "helper StopInstance failed, trying KillInstance";
        m_helperClient->killInstance(m_helperPid);
    }

    m_helperPid = 0;
    m_gamescopePid = 0;
    setStatus(QStringLiteral("Stopped"));
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

void GamescopeInstance::kill()
{
    if (m_helperPid <= 0) {
        return;
    }

    setStatus(QStringLiteral("Killing..."));
    if (m_helperClient) {
        m_helperClient->killInstance(m_helperPid);
    }

    m_helperPid = 0;
    m_gamescopePid = 0;
    setStatus(QStringLiteral("Killed"));
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

bool GamescopeInstance::isRunning() const
{
    return m_helperPid > 0;
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

void GamescopeInstance::onHelperInstanceStopped(const QString &username, qint64 pid, const QString &reason)
{
    Q_UNUSED(username)

    if (pid != m_helperPid) {
        return;
    }


    QString statusMsg;
    if (reason == QStringLiteral("crashed")) {
        statusMsg = QStringLiteral("Crashed");
    } else if (reason == QStringLiteral("failed")) {
        statusMsg = QStringLiteral("Failed");
    } else {
        statusMsg = QStringLiteral("Exited unexpectedly");
    }

    m_helperPid = 0;
    m_gamescopePid = 0;
    setStatus(statusMsg);
    Q_EMIT runningChanged();
    Q_EMIT stopped();
    Q_EMIT errorOccurred(statusMsg);
}
