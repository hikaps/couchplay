// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "GamescopeInstance.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>

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
    if (m_process && m_process->state() != QProcess::NotRunning) {
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

    // Steam arguments
    QString steamArgs = QStringLiteral("-gamepadui");
    QString gameCommand = config.value(QStringLiteral("gameCommand")).toString();
    if (!gameCommand.isEmpty()) {
        // Check if it's a Steam app ID or a command
        bool isAppId = false;
        gameCommand.toUInt(&isAppId);
        if (isAppId) {
            steamArgs += QStringLiteral(" -applaunch ") + gameCommand;
        } else if (gameCommand.startsWith(QStringLiteral("steam://"))) {
            // Steam URL, will be handled separately
            steamArgs += QStringLiteral(" ") + gameCommand;
        } else {
            steamArgs += QStringLiteral(" -applaunch ") + gameCommand;
        }
    }

    // Create process
    m_process = new QProcess(this);

    connect(m_process, &QProcess::started, this, &GamescopeInstance::onProcessStarted);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &GamescopeInstance::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &GamescopeInstance::onProcessError);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &GamescopeInstance::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &GamescopeInstance::onReadyReadStandardError);

    if (m_isPrimary || m_username.isEmpty()) {
        // Primary instance - run directly with modified environment
        QString program = QStringLiteral("/usr/bin/gamescope");
        
        // Add steam command after --
        gamescopeArgs << QStringLiteral("--") << QStringLiteral("/usr/bin/steam");
        gamescopeArgs << steamArgs.split(QLatin1Char(' '), Qt::SkipEmptyParts);

        // Set up environment
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &var : envVars) {
            int eqPos = var.indexOf(QLatin1Char('='));
            if (eqPos > 0) {
                env.insert(var.left(eqPos), var.mid(eqPos + 1));
            }
        }
        m_process->setProcessEnvironment(env);

        setStatus(QStringLiteral("Starting..."));
        qDebug() << "Starting gamescope:" << program << gamescopeArgs;
        m_process->start(program, gamescopeArgs);
    } else {
        // Secondary instance - run as different user via sudo
        QString command = buildSecondaryUserCommand(m_username, envVars, gamescopeArgs, steamArgs);

        setStatus(QStringLiteral("Starting as %1...").arg(m_username));
        qDebug() << "Starting secondary instance:" << command;
        m_process->start(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), command});
    }

    return true;
}

void GamescopeInstance::stop(int timeoutMs)
{
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

    m_process->deleteLater();
    m_process = nullptr;

    setStatus(QStringLiteral("Stopped"));
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

void GamescopeInstance::kill()
{
    if (!m_process) {
        return;
    }

    if (m_process->state() != QProcess::NotRunning) {
        setStatus(QStringLiteral("Killing..."));
        m_process->kill();
        m_process->waitForFinished(2000);
    }

    m_process->deleteLater();
    m_process = nullptr;

    setStatus(QStringLiteral("Killed"));
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

bool GamescopeInstance::isRunning() const
{
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

    // Borderless window (allows positioning)
    args << QStringLiteral("-b");

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
    int posX = config.value(QStringLiteral("positionX"), -1).toInt();
    int posY = config.value(QStringLiteral("positionY"), -1).toInt();
    if (posX >= 0 && posY >= 0) {
        args << QStringLiteral("--position") << QStringLiteral("%1,%2").arg(posX).arg(posY);
    }

    // Monitor selection (for multi-monitor setups)
    int monitor = config.value(QStringLiteral("monitor"), -1).toInt();
    if (monitor >= 0) {
        args << QStringLiteral("--prefer-output") << config.value(QStringLiteral("monitorName")).toString();
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

    // Disable global keyboard shortcuts to prevent interference
    // args << QStringLiteral("--keyboard-focus-inhibit");

    return args;
}

QStringList GamescopeInstance::buildEnvironment(const QVariantMap &config, bool isPrimary)
{
    QStringList env;

    // For secondary users, set up PipeWire audio forwarding via TCP
    if (!isPrimary) {
        // The primary user should have PipeWire configured to accept TCP connections
        // Secondary users connect via TCP to the primary's PipeWire
        QString pulseServer = config.value(QStringLiteral("pulseServer"), 
                                           QStringLiteral("tcp:127.0.0.1:4713")).toString();
        env << QStringLiteral("PULSE_SERVER=%1").arg(pulseServer);
    }

    // SDL should not use the default display server for input
    // This helps ensure input goes through gamescope
    env << QStringLiteral("SDL_VIDEODRIVER=x11");

    // Force Steam to use the gamescope display
    // env << QStringLiteral("STEAM_FORCE_DESKTOPUI_SCALING=1");

    return env;
}

QString GamescopeInstance::buildSecondaryUserCommand(const QString &username,
                                                       const QStringList &environment,
                                                       const QStringList &gamescopeArgs,
                                                       const QString &steamArgs)
{
    // Build environment string
    QString envStr;
    for (const QString &var : environment) {
        envStr += var + QStringLiteral(" ");
    }

    // We need to pass the current DISPLAY and XAUTHORITY to the secondary user
    // This requires proper setup (xhost or shared Xauthority)
    QString displayEnv = QStringLiteral("DISPLAY=$DISPLAY");
    
    // Build the gamescope command
    QString gamescopeCmd = QStringLiteral("/usr/bin/gamescope %1 -- /usr/bin/steam %2")
                               .arg(gamescopeArgs.join(QLatin1Char(' ')), steamArgs);

    QString command;

    // Check if machinectl is available (systemd-based, more secure)
    if (QFile::exists(QStringLiteral("/usr/bin/machinectl"))) {
        // Use machinectl shell for better isolation
        // This requires the user to be in the right groups
        command = QStringLiteral("sudo /usr/bin/machinectl shell %1@ /bin/bash -c '%2 %3 %4'")
                      .arg(username, envStr, displayEnv, gamescopeCmd);
    } else if (QFile::exists(QStringLiteral("/usr/bin/run-as"))) {
        // Use run-as if available (provides X forwarding)
        command = QStringLiteral("sudo /usr/bin/run-as -X %1 -- env %2 %3 %4")
                      .arg(username, envStr, displayEnv, gamescopeCmd);
    } else {
        // Fallback to sudo -u
        // Note: This may require additional xhost configuration
        command = QStringLiteral("sudo -u %1 env %2 %3 %4")
                      .arg(username, envStr, displayEnv, gamescopeCmd);
    }

    return command;
}

void GamescopeInstance::onProcessStarted()
{
    setStatus(QStringLiteral("Running"));
    Q_EMIT runningChanged();
    Q_EMIT started();
    qDebug() << "Instance" << m_index << "started with PID" << pid();
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
    qDebug() << "Instance" << m_index << "finished:" << statusStr;
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
