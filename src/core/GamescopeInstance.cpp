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
}

bool GamescopeInstance::start(const QVariantMap &config, int index)
{
    if (m_helperPid > 0) {
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
    
    if (gameCommand.isEmpty()) {
        // Fallback to Steam Big Picture if no preset configured
        gameCommand = QStringLiteral("steam -tenfoot -steamdeck");
    }
    
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
        return false;
    }
    
    // Use the helper service to launch the instance
    // compositorUid is the UID of the user running CouchPlay (owns the Wayland socket)
    uid_t compositorUid = getuid();
    
    QStringList bindPaths = config.value(QStringLiteral("bindPaths")).toStringList();
    
    QDBusReply<qint64> reply = helper.call(
        QStringLiteral("LaunchInstance"),
        m_username,
        static_cast<uint>(compositorUid),
        gamescopeArgs,
        gameCommand,
        envVars,
        bindPaths
    );
    
    if (!reply.isValid()) {
        qWarning() << "Instance" << m_index << "helper LaunchInstance failed:" << reply.error().message();
        Q_EMIT errorOccurred(QStringLiteral("Failed to launch instance: %1").arg(reply.error().message()));
        return false;
    }
    
    m_helperPid = reply.value();
    if (m_helperPid == 0) {
        qWarning() << "Instance" << m_index << "helper returned PID 0";
        Q_EMIT errorOccurred(QStringLiteral("Helper service failed to launch instance"));
        return false;
    }
    
    m_gamescopePid = resolveGamescopePid(m_helperPid);
    if (m_gamescopePid == 0) {
        qWarning() << "Instance" << m_index << "could not resolve gamescope PID from helper PID" << m_helperPid;
        m_gamescopePid = m_helperPid;
    }
    Q_EMIT gamescopePidChanged();
    setStatus(QStringLiteral("Running as %1").arg(m_username));

    QDBusConnection::systemBus().connect(
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QStringLiteral("instanceStopped"),
        this, SLOT(onHelperInstanceStopped(QString, qint64, QString))
    );
    
    // Signal running immediately since helper-launched instances don't use QProcess signals
    Q_EMIT runningChanged();
    Q_EMIT started();

    return true;
}

void GamescopeInstance::stop(int timeoutMs)
{
    Q_UNUSED(timeoutMs)

    // Handle helper-launched instances
    if (m_helperPid > 0) {
        setStatus(QStringLiteral("Stopping..."));

        QDBusConnection::systemBus().disconnect(
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("instanceStopped"),
            this, SLOT(onHelperInstanceStopped(QString, qint64, QString))
        );

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

        m_helperPid = 0;
        m_gamescopePid = 0;
        setStatus(QStringLiteral("Stopped"));
        Q_EMIT runningChanged();
        Q_EMIT stopped();
        return;
    }
}

void GamescopeInstance::kill()
{
    // Handle helper-launched instances
    if (m_helperPid > 0) {
        setStatus(QStringLiteral("Killing..."));

        QDBusConnection::systemBus().disconnect(
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("instanceStopped"),
            this, SLOT(onHelperInstanceStopped(QString, qint64, QString))
        );

        QDBusInterface helper(
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QDBusConnection::systemBus()
        );

        if (helper.isValid()) {
            helper.call(QStringLiteral("KillInstance"), m_helperPid);
        }

        m_helperPid = 0;
        m_gamescopePid = 0;
        setStatus(QStringLiteral("Killed"));
        Q_EMIT runningChanged();
        Q_EMIT stopped();
        return;
    }
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

qint64 GamescopeInstance::resolveGamescopePid(qint64 launchedPid)
{
    if (launchedPid <= 0) {
        return 0;
    }

    QString childrenPath = QStringLiteral("/proc/%1/task/%1/children").arg(launchedPid);
    QFile childrenFile(childrenPath);
    if (!childrenFile.open(QIODevice::ReadOnly)) {
        return 0;
    }

    QStringList childPids = QString::fromLocal8Bit(childrenFile.readAll())
                                .split(QLatin1Char(' '), Qt::SkipEmptyParts);

    for (const QString &childPidStr : childPids) {
        qint64 childPid = childPidStr.toLongLong();
        if (childPid <= 0) {
            continue;
        }

        QString commPath = QStringLiteral("/proc/%1/comm").arg(childPid);
        QFile commFile(commPath);
        if (commFile.open(QIODevice::ReadOnly)) {
            QString comm = QString::fromLocal8Bit(commFile.readAll()).trimmed();
            if (comm == QLatin1String("gamescope")) {
                return childPid;
            }
        }
    }

    return 0;
}

void GamescopeInstance::onHelperInstanceStopped(const QString &username, qint64 pid, const QString &reason)
{
    Q_UNUSED(username)

    if (pid != m_helperPid) {
        return;
    }

    QDBusConnection::systemBus().disconnect(
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
        QStringLiteral("io.github.hikaps.CouchPlayHelper"),
        QStringLiteral("instanceStopped"),
        this, SLOT(onHelperInstanceStopped(QString, qint64, QString))
    );

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
