// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "GamescopeInstance.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
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
    cleanupWaylandAccess();
}

bool GamescopeInstance::start(const QVariantMap &config, int index)
{
    if (m_helperPid > 0 || (m_process && m_process->state() != QProcess::NotRunning)) {
        Q_EMIT errorOccurred(QStringLiteral("Instance already running"));
        return false;
    }

    m_index = index;
    m_username = config.value(QStringLiteral("username")).toString();

    // Store window geometry for UI display
    int posX = config.value(QStringLiteral("positionX"), 0).toInt();
    int posY = config.value(QStringLiteral("positionY"), 0).toInt();
    int outputW = config.value(QStringLiteral("outputWidth"), 960).toInt();
    int outputH = config.value(QStringLiteral("outputHeight"), 1080).toInt();
    m_windowGeometry = QRect(posX, posY, outputW, outputH);
    Q_EMIT configChanged();

    // Build gamescope arguments
    QStringList gamescopeArgs = buildGamescopeArgs(config);

    // Build environment
    QStringList envVars = buildEnvironment(config);

    // Get preset command from SessionRunner (resolved from PresetManager)
    // Fallback to Steam Big Picture if no preset command provided
    QString gameCommand = config.value(QStringLiteral("presetCommand")).toString();
    QString workingDirectory = config.value(QStringLiteral("presetWorkingDirectory")).toString();
    
    if (gameCommand.isEmpty()) {
        // Fallback to Steam Big Picture if no preset configured
        gameCommand = QStringLiteral("steam -tenfoot -steamdeck");
    }
    
    // Verify we have a command to run
    if (gameCommand.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("No game command configured"));
        return false;
    }

    // Create process
    m_process = new QProcess(this);

    connect(m_process, &QProcess::started, this, &GamescopeInstance::onProcessStarted);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &GamescopeInstance::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &GamescopeInstance::onProcessError);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &GamescopeInstance::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &GamescopeInstance::onReadyReadStandardError);

    // All instances go through the D-Bus helper service
    // This provides uniform handling for all users, including the compositor user
    
    // Use the D-Bus helper to launch the instance
    QDBusInterface helper(
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QDBusConnection::systemBus()
    );
    
    if (!helper.isValid()) {
        qWarning() << "Instance" << m_index << "helper service not available";
        Q_EMIT errorOccurred(QStringLiteral("CouchPlay Helper service is not available. Please run: sudo ./scripts/install-helper.sh install"));
        m_process->deleteLater();
        m_process = nullptr;
        return false;
    }
    
    // Use the helper service to launch the instance
    // compositorUid is the UID of the user running CouchPlay (owns the Wayland socket)
    uid_t compositorUid = getuid();
    
    QDBusReply<qint64> reply = helper.call(
        QStringLiteral("LaunchInstance"),
        m_username,
        static_cast<uint>(compositorUid),
        gamescopeArgs,
        gameCommand,
        envVars
    );
    
    if (!reply.isValid()) {
        qWarning() << "Instance" << m_index << "helper LaunchInstance failed:" << reply.error().message();
        Q_EMIT errorOccurred(QStringLiteral("Failed to launch instance: %1").arg(reply.error().message()));
        m_process->deleteLater();
        m_process = nullptr;
        return false;
    }
    
    m_helperPid = reply.value();
    if (m_helperPid == 0) {
        qWarning() << "Instance" << m_index << "helper returned PID 0";
        Q_EMIT errorOccurred(QStringLiteral("Helper service failed to launch instance"));
        m_process->deleteLater();
        m_process = nullptr;
        return false;
    }
    
    setStatus(QStringLiteral("Running as %1").arg(m_username));
    
    // Signal running immediately since helper-launched instances don't use QProcess signals
    Q_EMIT runningChanged();
    Q_EMIT started();

    return true;
}

void GamescopeInstance::stop(int timeoutMs)
{
    // Handle helper-launched instances
    if (m_helperPid > 0) {
        setStatus(QStringLiteral("Stopping..."));
        
        QDBusInterface helper(
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QDBusConnection::systemBus()
        );
        
        if (helper.isValid()) {
            QDBusReply<bool> reply = helper.call(QStringLiteral("StopInstance"), m_helperPid);
            if (!reply.isValid() || !reply.value()) {
                qWarning() << "Instance" << m_index << "helper StopInstance failed, trying KillInstance";
                helper.call(QStringLiteral("KillInstance"), m_helperPid);
            }
        }
        
        // Clean up Wayland ACLs for secondary user
        cleanupWaylandAccess();
        
        m_helperPid = 0;
        setStatus(QStringLiteral("Stopped"));
        Q_EMIT runningChanged();
        Q_EMIT stopped();
        return;
    }

    if (!m_process) {
        return;
    }

    if (m_process->state() != QProcess::NotRunning) {
        setStatus(QStringLiteral("Stopping..."));

        m_process->terminate();

        if (!m_process->waitForFinished(timeoutMs)) {
            qWarning() << "Instance" << m_index << "did not stop gracefully, killing...";
            m_process->kill();
            m_process->waitForFinished(2000);
        }
    }

    // Clean up Wayland ACLs for secondary user
    cleanupWaylandAccess();

    m_process->deleteLater();
    m_process = nullptr;

    setStatus(QStringLiteral("Stopped"));
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

void GamescopeInstance::kill()
{
    // Handle helper-launched instances
    if (m_helperPid > 0) {
        setStatus(QStringLiteral("Killing..."));
        
        QDBusInterface helper(
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QDBusConnection::systemBus()
        );
        
        if (helper.isValid()) {
            helper.call(QStringLiteral("KillInstance"), m_helperPid);
        }
        
        // Clean up Wayland ACLs for secondary user
        cleanupWaylandAccess();
        
        m_helperPid = 0;
        setStatus(QStringLiteral("Killed"));
        Q_EMIT runningChanged();
        Q_EMIT stopped();
        return;
    }

    if (!m_process) {
        return;
    }

    if (m_process->state() != QProcess::NotRunning) {
        setStatus(QStringLiteral("Killing..."));
        m_process->kill();
        m_process->waitForFinished(2000);
    }

    // Clean up Wayland ACLs for secondary user
    cleanupWaylandAccess();

    m_process->deleteLater();
    m_process = nullptr;

    setStatus(QStringLiteral("Killed"));
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

bool GamescopeInstance::isRunning() const
{
    // Check helper-launched instances
    if (m_helperPid > 0) {
        return true;  // Assume running if we have a PID (we can't easily check)
    }
    return m_process && m_process->state() == QProcess::Running;
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

    // Steam integration - expose Steam-specific window properties
    // Only enable for presets that require it (e.g., Steam Big Picture mode)
    bool steamIntegration = config.value(QStringLiteral("steamIntegration"), false).toBool();
    if (steamIntegration) {
        args << QStringLiteral("-e");
    }

    // Borderless window (optional - allows positioning but removes decorations)
    // Default is false (decorated windows for resizing)
    bool borderless = config.value(QStringLiteral("borderless"), false).toBool();
    if (borderless) {
        args << QStringLiteral("-b");
    }

    // Note: Don't pass --backend flag - let gamescope auto-detect
    // It will use wayland backend when WAYLAND_DISPLAY is set

    // Internal resolution (game renders at this)
    int internalW = config.value(QStringLiteral("internalWidth"), 1920).toInt();
    int internalH = config.value(QStringLiteral("internalHeight"), 1080).toInt();
    args << QStringLiteral("-w") << QString::number(internalW);
    args << QStringLiteral("-h") << QString::number(internalH);

    // Output resolution (window size)
    int outputW = config.value(QStringLiteral("outputWidth"), 960).toInt();
    int outputH = config.value(QStringLiteral("outputHeight"), 1080).toInt();
    args << QStringLiteral("-W") << QString::number(outputW);
    args << QStringLiteral("-H") << QString::number(outputH);

    // Refresh rate
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

    // Window position for split-screen layouts
    // NOTE: --position is not available in all gamescope versions
    // For now, we skip positioning and let the window manager handle it
    // TODO: Investigate using xdotool/wmctrl for window positioning after spawn
    // int posX = config.value(QStringLiteral("positionX"), -1).toInt();
    // int posY = config.value(QStringLiteral("positionY"), -1).toInt();
    // if (posX >= 0 && posY >= 0) {
    //     args << QStringLiteral("--position") << QStringLiteral("%1,%2").arg(posX).arg(posY);
    // }

    // Monitor selection (for multi-monitor setups)
    QString monitorName = config.value(QStringLiteral("monitorName")).toString();
    if (!monitorName.isEmpty()) {
        args << QStringLiteral("--prefer-output") << monitorName;
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
    Q_UNUSED(config)
    
    QStringList envVars;
    
    // Enable Gamescope WSI layer - critical for Vulkan games to work inside gamescope
    envVars << QStringLiteral("ENABLE_GAMESCOPE_WSI=1");
    
    // Prevent games from minimizing when losing focus
    envVars << QStringLiteral("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0");
    
    // Mesa threading for better performance
    envVars << QStringLiteral("mesa_glthread=true");
    
    // Set desktop environment for XDG portal integration
    // This enables native file dialogs in Steam and other applications
    envVars << QStringLiteral("XDG_CURRENT_DESKTOP=KDE");
    
    // Force GTK applications to use XDG portals for file dialogs
    envVars << QStringLiteral("GTK_USE_PORTAL=1");
    
    return envVars;
}

void GamescopeInstance::onProcessStarted()
{
    setStatus(QStringLiteral("Running"));
    Q_EMIT runningChanged();
    Q_EMIT started();
}

void GamescopeInstance::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QString statusStr;
    if (exitStatus == QProcess::CrashExit) {
        statusStr = QStringLiteral("Crashed (code %1)").arg(exitCode);
    } else if (exitCode == 0) {
        statusStr = QStringLiteral("Exited normally");
    } else {
        statusStr = QStringLiteral("Exited (code %1)").arg(exitCode);
    }

    setStatus(statusStr);
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

void GamescopeInstance::onProcessError(QProcess::ProcessError error)
{
    QString errorMsg;
    switch (error) {
    case QProcess::FailedToStart:
        errorMsg = QStringLiteral("Failed to start gamescope - is it installed?");
        break;
    case QProcess::Crashed:
        errorMsg = QStringLiteral("Gamescope crashed unexpectedly");
        break;
    case QProcess::Timedout:
        errorMsg = QStringLiteral("Gamescope operation timed out");
        break;
    case QProcess::WriteError:
        errorMsg = QStringLiteral("Failed to write to gamescope process");
        break;
    case QProcess::ReadError:
        errorMsg = QStringLiteral("Failed to read from gamescope process");
        break;
    default:
        errorMsg = QStringLiteral("Unknown process error");
    }

    setStatus(errorMsg);
    Q_EMIT errorOccurred(errorMsg);
    qWarning() << "Instance" << m_index << "error:" << errorMsg;
}

void GamescopeInstance::onReadyReadStandardOutput()
{
    QString output = QString::fromUtf8(m_process->readAllStandardOutput());
    if (!output.trimmed().isEmpty()) {
        Q_EMIT outputReceived(output);
    }
}

void GamescopeInstance::onReadyReadStandardError()
{
    QString output = QString::fromUtf8(m_process->readAllStandardError());
    if (!output.trimmed().isEmpty()) {
        Q_EMIT outputReceived(QStringLiteral("[stderr] ") + output);
    }
}

bool GamescopeInstance::setupWaylandAccessForUser(const QString &username)
{
    // Use the D-Bus helper service to set up Wayland ACLs
    // This requires root privileges, which the helper has
    
    QDBusInterface helper(
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QDBusConnection::systemBus()
    );
    
    if (!helper.isValid()) {
        qWarning() << "CouchPlay helper service not available for Wayland access setup";
        // Fallback: try to set ACLs directly (will fail if not root)
        return setupWaylandAccessFallback(username);
    }
    
    // Call SetupWaylandAccess(username, compositorUid)
    uid_t compositorUid = getuid();
    QDBusReply<bool> reply = helper.call(
        QStringLiteral("SetupWaylandAccess"), 
        username, 
        static_cast<uint>(compositorUid)
    );
    
    if (!reply.isValid()) {
        qWarning() << "Failed to call SetupWaylandAccess:" << reply.error().message();
        return setupWaylandAccessFallback(username);
    }
    
    if (!reply.value()) {
        qWarning() << "SetupWaylandAccess returned false";
        return false;
    }
    
    m_waylandAclSet = true;
    return true;
}

bool GamescopeInstance::setupWaylandAccessFallback(const QString &username)
{
    // Fallback: try to set ACLs directly (will only work if already have permissions)
    QString runtimeDir = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    if (runtimeDir.isEmpty()) {
        runtimeDir = QStringLiteral("/run/user/%1").arg(getuid());
    }
    
    QString waylandDisplay = QString::fromLocal8Bit(qgetenv("WAYLAND_DISPLAY"));
    if (waylandDisplay.isEmpty()) {
        waylandDisplay = QStringLiteral("wayland-0");
    }
    
    QString waylandSocket = runtimeDir + QStringLiteral("/") + waylandDisplay;
    
    if (!QFile::exists(waylandSocket)) {
        qWarning() << "Wayland socket not found:" << waylandSocket;
        return false;
    }
    
    // Try to set ACL on runtime dir (traverse permission)
    QProcess setfaclDir;
    setfaclDir.start(QStringLiteral("setfacl"), 
        {QStringLiteral("-m"), QStringLiteral("u:%1:x").arg(username), runtimeDir});
    setfaclDir.waitForFinished(5000);
    
    if (setfaclDir.exitCode() != 0) {
        qWarning() << "Fallback: Failed to set ACL on runtime dir:" << setfaclDir.readAllStandardError();
        return false;
    }
    
    // Try to set ACL on Wayland socket (rw permission)
    QProcess setfaclSocket;
    setfaclSocket.start(QStringLiteral("setfacl"), 
        {QStringLiteral("-m"), QStringLiteral("u:%1:rw").arg(username), waylandSocket});
    setfaclSocket.waitForFinished(5000);
    
    if (setfaclSocket.exitCode() != 0) {
        qWarning() << "Fallback: Failed to set ACL on Wayland socket:" << setfaclSocket.readAllStandardError();
        // Clean up
        QProcess::execute(QStringLiteral("setfacl"), 
            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(username), runtimeDir});
        return false;
    }
    
    m_waylandAclSet = true;
    return true;
}

void GamescopeInstance::cleanupWaylandAccess()
{
    if (!m_waylandAclSet || m_username.isEmpty()) {
        return;
    }
    
    // Use the D-Bus helper service to remove Wayland ACLs
    QDBusInterface helper(
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QDBusConnection::systemBus()
    );
    
    uid_t compositorUid = getuid();
    
    if (helper.isValid()) {
        QDBusReply<bool> reply = helper.call(
            QStringLiteral("RemoveWaylandAccess"), 
            m_username, 
            static_cast<uint>(compositorUid)
        );
        
        if (reply.isValid() && reply.value()) {
            m_waylandAclSet = false;
            return;
        }
        qWarning() << "Helper RemoveWaylandAccess failed, trying fallback";
    }
    
    // Fallback: try direct setfacl (may fail without permissions)
    QString runtimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);
    QString waylandSocket = runtimeDir + QStringLiteral("/wayland-0");
    
    if (QFile::exists(waylandSocket)) {
        QProcess::execute(QStringLiteral("setfacl"), 
            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(m_username), waylandSocket});
    }
    
    if (QFile::exists(runtimeDir)) {
        QProcess::execute(QStringLiteral("setfacl"), 
            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(m_username), runtimeDir});
    }
    
    m_waylandAclSet = false;
}
