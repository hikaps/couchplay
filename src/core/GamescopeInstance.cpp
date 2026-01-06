// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "GamescopeInstance.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryFile>

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
    m_isPrimary = (index == 0);
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
    QStringList envVars = buildEnvironment(config, m_isPrimary);

    // Get launch mode and game configuration
    // Default to "steam" mode - players select games inside Steam Big Picture
    QString launchMode = config.value(QStringLiteral("launchMode"), QStringLiteral("steam")).toString();
    QString steamAppId = config.value(QStringLiteral("steamAppId")).toString();
    QString executablePath = config.value(QStringLiteral("executablePath")).toString();
    QString protonPath = config.value(QStringLiteral("protonPath")).toString();
    QString prefixPath = config.value(QStringLiteral("prefixPath")).toString();
    QString workingDirectory = config.value(QStringLiteral("workingDirectory")).toString();
    
    QString gameCommand;
    
    if (launchMode == QStringLiteral("steam")) {
        // Steam launch mode - launch Steam Big Picture, optionally with a specific game
        // -tenfoot launches Steam in Big Picture mode (better for controllers)
        // -steamdeck enables Steam Deck compatibility for proper Proton prefix handling
        if (steamAppId.isEmpty()) {
            // Just launch Steam Big Picture - player will select game inside Steam
            gameCommand = QStringLiteral("steam -tenfoot -steamdeck");
            qDebug() << "Instance" << m_index << "launching Steam Big Picture";
        } else {
            // Launch specific game via Steam
            gameCommand = QStringLiteral("steam -tenfoot -steamdeck steam://rungameid/%1").arg(steamAppId);
            qDebug() << "Instance" << m_index << "launching Steam game" << steamAppId;
        }
    } else if (launchMode == QStringLiteral("direct")) {
        // Direct launch mode - launch executable directly (with Proton if needed)
        if (executablePath.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("No executable path specified"));
            return false;
        }
        
        // Check if executable exists
        if (!QFile::exists(executablePath)) {
            Q_EMIT errorOccurred(QStringLiteral("Executable not found: %1").arg(executablePath));
            return false;
        }
        
        // Determine if this is a Windows game (needs Proton) or native
        bool isWindowsGame = executablePath.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive);
        
        if (isWindowsGame) {
            // Windows game - use Proton
            if (protonPath.isEmpty()) {
                Q_EMIT errorOccurred(QStringLiteral("Proton path required for Windows games"));
                return false;
            }
            if (!QFile::exists(protonPath)) {
                Q_EMIT errorOccurred(QStringLiteral("Proton not found: %1").arg(protonPath));
                return false;
            }
            
            // Default prefix if not specified
            if (prefixPath.isEmpty()) {
                prefixPath = QDir::homePath() + QStringLiteral("/.couchplay/prefixes/default");
                QDir().mkpath(prefixPath);
            }
            
            // Add Proton environment variables
            envVars << QStringLiteral("STEAM_COMPAT_DATA_PATH=%1").arg(prefixPath);
            envVars << QStringLiteral("STEAM_COMPAT_CLIENT_INSTALL_PATH=%1/.local/share/Steam").arg(QDir::homePath());
            
            // Build Proton command: proton run /path/to/game.exe
            gameCommand = QStringLiteral("\"%1\" run \"%2\"").arg(protonPath, executablePath);
        } else {
            // Native Linux game - run directly
            gameCommand = QStringLiteral("\"%1\"").arg(executablePath);
        }
        
        qDebug() << "Instance" << m_index << "using direct launch mode";
    } else {
        // Unknown launch mode - default to Steam
        gameCommand = QStringLiteral("steam -tenfoot -steamdeck");
        qDebug() << "Instance" << m_index << "unknown launch mode, defaulting to Steam Big Picture";
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

    // Get current username to determine if this is really a "primary" path
    // (running as the current user) vs "secondary" path (running as a different user)
    QString currentUser = QString::fromLocal8Bit(qgetenv("USER"));
    bool useSecondaryPath = !m_isPrimary && !m_username.isEmpty() && m_username != currentUser;
    
    qDebug() << "Instance" << m_index << "starting:"
             << "user=" << m_username
             << "path=" << (useSecondaryPath ? "secondary" : "primary");
    
    if (!useSecondaryPath) {
        // Primary instance - run directly with modified environment
        QString program = QStandardPaths::findExecutable(QStringLiteral("gamescope"));
        if (program.isEmpty()) {
            // Fallback to common paths
            for (const QString &path : {QStringLiteral("/usr/bin/gamescope"), 
                                         QStringLiteral("/usr/local/bin/gamescope")}) {
                if (QFile::exists(path)) {
                    program = path;
                    break;
                }
            }
        }
        if (program.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("gamescope not found - please install gamescope"));
            return false;
        }
        
        // Add game command after --
        gamescopeArgs << QStringLiteral("--");
        
        // For Proton, we need to run through bash to handle the command properly
        gamescopeArgs << QStringLiteral("/bin/bash") << QStringLiteral("-c") << gameCommand;

        // Set up environment
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &var : envVars) {
            int eqPos = var.indexOf(QLatin1Char('='));
            if (eqPos > 0) {
                env.insert(var.left(eqPos), var.mid(eqPos + 1));
            }
        }
        m_process->setProcessEnvironment(env);
        
        // Set working directory if specified
        if (!workingDirectory.isEmpty() && QDir(workingDirectory).exists()) {
            m_process->setWorkingDirectory(workingDirectory);
        } else if (!executablePath.isEmpty()) {
            // Default to executable's directory
            m_process->setWorkingDirectory(QFileInfo(executablePath).absolutePath());
        }

        setStatus(QStringLiteral("Starting..."));
        m_process->start(program, gamescopeArgs);
        
        if (!m_process->waitForStarted(3000)) {
            qWarning() << "Instance" << m_index << "failed to start:" << m_process->errorString();
        }
    } else {
        // Secondary instance - run as different user via helper service
        // The helper handles all the complexity of machinectl, environment setup, etc.
        
        qDebug() << "Instance" << m_index << "starting as" << m_username << "via helper service";
        
        // Use the D-Bus helper to launch the instance
        QDBusInterface helper(
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("/io/github/hikaps/CouchPlayHelper"),
            QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QDBusConnection::systemBus()
        );
        
        if (!helper.isValid()) {
            qWarning() << "Instance" << m_index << "helper service not available, falling back to direct launch";
            
            // Fallback: try the old method (build command and run via bash)
            if (!setupWaylandAccessForUser(m_username)) {
                qWarning() << "Instance" << m_index << "failed to set up Wayland access for" << m_username;
                Q_EMIT errorOccurred(QStringLiteral("Failed to set up Wayland access for %1").arg(m_username));
            }
            
            QString command = buildSecondaryUserCommand(m_username, envVars, gamescopeArgs, gameCommand);
            setStatus(QStringLiteral("Starting as %1...").arg(m_username));
            m_process->start(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), command});
        } else {
            // Use the helper service to launch the instance
            uid_t primaryUid = getuid();
            
            QDBusReply<qint64> reply = helper.call(
                QStringLiteral("LaunchInstance"),
                m_username,
                static_cast<uint>(primaryUid),
                gamescopeArgs,
                gameCommand,
                envVars
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
            
            qDebug() << "Instance" << m_index << "launched via helper with PID" << m_helperPid;
            setStatus(QStringLiteral("Running as %1").arg(m_username));
            
            // We don't have a QProcess for helper-launched instances, so signal running immediately
            Q_EMIT runningChanged();
            Q_EMIT started();
        }
    }

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
    args << QStringLiteral("-e");

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

    // Input device isolation - critical for split-screen
    QVariantList devicePaths = config.value(QStringLiteral("devicePaths")).toList();
    for (const QVariant &path : devicePaths) {
        QString devicePath = path.toString();
        if (!devicePath.isEmpty() && QFile::exists(devicePath)) {
            args << QStringLiteral("--input-device") << devicePath;
        }
    }

    // If we have input devices, enable grab mode
    if (!devicePaths.isEmpty()) {
        args << QStringLiteral("--grab");
    }

    return args;
}

QStringList GamescopeInstance::buildEnvironment(const QVariantMap &config, bool isPrimary)
{
    Q_UNUSED(config)
    Q_UNUSED(isPrimary)
    
    QStringList envVars;
    
    // Enable Gamescope WSI layer - critical for Vulkan games to work inside gamescope
    envVars << QStringLiteral("ENABLE_GAMESCOPE_WSI=1");
    
    // Prevent games from minimizing when losing focus
    envVars << QStringLiteral("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0");
    
    // Mesa threading for better performance
    envVars << QStringLiteral("mesa_glthread=true");
    
    return envVars;
}

QString GamescopeInstance::buildSecondaryUserCommand(const QString &username,
                                                       const QStringList &environment,
                                                       const QStringList &gamescopeArgs,
                                                       const QString &gameCommand)
{
    // Build environment exports for the secondary user
    // Key insight: Let the secondary user use their OWN XDG_RUNTIME_DIR
    // (so gamescope can create lockfiles there), but point WAYLAND_DISPLAY
    // to the primary user's Wayland socket as an absolute path.
    
    QStringList exports;
    
    // Get the primary user's UID for the Wayland socket path
    uid_t primaryUid = getuid();
    QString primaryRuntimeDir = QStringLiteral("/run/user/%1").arg(primaryUid);
    QString primaryWaylandSocket = primaryRuntimeDir + QStringLiteral("/wayland-0");
    
    // Set WAYLAND_DISPLAY to the absolute path of the primary user's Wayland socket
    // The secondary user has ACL access to this socket (set up by helper)
    exports << QStringLiteral("export WAYLAND_DISPLAY=%1").arg(primaryWaylandSocket);
    
    // Set DISPLAY for XWayland compatibility
    QString display = QString::fromLocal8Bit(qgetenv("DISPLAY"));
    if (!display.isEmpty()) {
        exports << QStringLiteral("export DISPLAY=%1").arg(display);
    }
    
    // Set XAUTHORITY - needed for X11/XWayland authentication
    // The secondary user needs access to the primary user's Xauthority file
    QString xauthority = QString::fromLocal8Bit(qgetenv("XAUTHORITY"));
    if (xauthority.isEmpty()) {
        // Default XAUTHORITY location
        xauthority = QDir::homePath() + QStringLiteral("/.Xauthority");
    }
    if (QFile::exists(xauthority)) {
        exports << QStringLiteral("export XAUTHORITY=%1").arg(xauthority);
    }
    
    // NOTE: We do NOT override XDG_RUNTIME_DIR here!
    // The secondary user should use their own runtime dir (e.g., /run/user/1001)
    // so gamescope can create its lockfiles and sockets there.
    // Only the Wayland socket connection goes to the primary user's socket.
    
    // For audio, point to the primary user's PipeWire socket
    // PipeWire uses PIPEWIRE_RUNTIME_DIR if set, otherwise XDG_RUNTIME_DIR
    exports << QStringLiteral("export PIPEWIRE_RUNTIME_DIR=%1").arg(primaryRuntimeDir);
    
    // Add any other environment variables from buildEnvironment()
    for (const QString &var : environment) {
        exports << QStringLiteral("export %1").arg(var);
    }

    // Build the gamescope command
    // Log file helps capture errors for debugging
    QString logFile = QStringLiteral("/tmp/couchplay-%1.log").arg(username);
    
    // Escape the game command for embedding in double quotes inside bash -c
    // We need to escape: double quotes, dollar signs, and backticks
    QString gameCommandForBash = gameCommand;
    gameCommandForBash.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    gameCommandForBash.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    gameCommandForBash.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    
    QString gamescopeCmd = QStringLiteral("/usr/bin/gamescope %1 -- /bin/bash -c \"%2\" 2>&1 | tee %3")
                               .arg(gamescopeArgs.join(QLatin1Char(' ')), gameCommandForBash, logFile);

    QString command;

    // Build the full command with exports followed by gamescope
    QString exportStr = exports.join(QStringLiteral("; "));
    
    // Escape the entire gamescopeCmd for embedding in single quotes
    // Replace ' with '\'' (end quote, escaped quote, start quote)
    QString escapedGamescopeCmd = gamescopeCmd;
    escapedGamescopeCmd.replace(QLatin1Char('\''), QStringLiteral("'\\''"));

    // Use machinectl shell to run in the user's systemd session
    // This requires linger to be enabled for the user (done by CreateUser)
    // The user's session provides their own XDG_RUNTIME_DIR at /run/user/<uid>/
    if (QFile::exists(QStringLiteral("/usr/bin/machinectl"))) {
        // machinectl shell runs the command within the user's systemd user session
        // We use 'export VAR=value; export VAR2=value2; command' syntax
        // to properly set environment variables for bash -c
        command = QStringLiteral("machinectl shell %1@ /bin/bash -c '%2; %3'")
                      .arg(username, exportStr, escapedGamescopeCmd);
    } else {
        // Fallback: pkexec with runuser (original approach)
        // This is less reliable but might work on non-systemd systems
        command = QStringLiteral("pkexec /usr/bin/runuser -u %1 -- /bin/bash -c '%2; %3'")
                      .arg(username, exportStr, escapedGamescopeCmd);
    }

    return command;
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

    // Log process exit for debugging
    qDebug() << "Instance" << m_index << "finished:" << statusStr;
    
    // Capture any remaining output for debugging
    if (m_process) {
        QByteArray stdErr = m_process->readAllStandardError();
        QByteArray stdOut = m_process->readAllStandardOutput();
        if (!stdErr.isEmpty()) {
            qDebug() << "Instance" << m_index << "stderr:" << QString::fromUtf8(stdErr).left(500);
        }
        if (!stdOut.isEmpty()) {
            qDebug() << "Instance" << m_index << "stdout:" << QString::fromUtf8(stdOut).left(500);
        }
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
    
    // Call SetupWaylandAccess(username, primaryUid)
    uid_t primaryUid = getuid();
    QDBusReply<bool> reply = helper.call(
        QStringLiteral("SetupWaylandAccess"), 
        username, 
        static_cast<uint>(primaryUid)
    );
    
    if (!reply.isValid()) {
        qWarning() << "Failed to call SetupWaylandAccess:" << reply.error().message();
        return setupWaylandAccessFallback(username);
    }
    
    if (!reply.value()) {
        qWarning() << "SetupWaylandAccess returned false";
        return false;
    }
    
    qDebug() << "Set up Wayland access for" << username << "via helper service";
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
    
    qDebug() << "Set up Wayland access for" << username << "via direct setfacl (fallback)";
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
    
    uid_t primaryUid = getuid();
    
    if (helper.isValid()) {
        QDBusReply<bool> reply = helper.call(
            QStringLiteral("RemoveWaylandAccess"), 
            m_username, 
            static_cast<uint>(primaryUid)
        );
        
        if (reply.isValid() && reply.value()) {
            qDebug() << "Removed Wayland access for" << m_username << "via helper service";
            m_waylandAclSet = false;
            return;
        }
        qWarning() << "Helper RemoveWaylandAccess failed, trying fallback";
    }
    
    // Fallback: try direct setfacl (may fail without permissions)
    QString runtimeDir = QStringLiteral("/run/user/%1").arg(primaryUid);
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
