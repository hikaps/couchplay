// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "GamescopeInstance.h"

#include <QDebug>
#include <QDir>

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

    // Build gamescope arguments
    QStringList gamescopeArgs = buildGamescopeArgs(config);

    // Steam arguments
    QString steamArgs = QStringLiteral("-gamepadui");
    QString gameCommand = config.value(QStringLiteral("gameCommand")).toString();
    if (!gameCommand.isEmpty()) {
        steamArgs += QStringLiteral(" -applaunch ") + gameCommand;
    }

    // Create process
    m_process = new QProcess(this);

    connect(m_process, &QProcess::started, this, &GamescopeInstance::onProcessStarted);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &GamescopeInstance::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &GamescopeInstance::onProcessError);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &GamescopeInstance::onReadyReadStandardOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, &GamescopeInstance::onReadyReadStandardError);

    QString username = config.value(QStringLiteral("username")).toString();

    if (m_isPrimary || username.isEmpty()) {
        // Primary instance - run directly
        QString program = QStringLiteral("/usr/bin/gamescope");
        gamescopeArgs << QStringLiteral("--") << QStringLiteral("/usr/bin/steam");
        gamescopeArgs << steamArgs.split(QLatin1Char(' '), Qt::SkipEmptyParts);

        m_status = QStringLiteral("Starting...");
        Q_EMIT statusChanged();

        m_process->start(program, gamescopeArgs);
    } else {
        // Secondary instance - run as different user
        QString command = buildSecondaryUserCommand(username, gamescopeArgs, steamArgs);

        m_status = QStringLiteral("Starting as %1...").arg(username);
        Q_EMIT statusChanged();

        m_process->start(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), command});
    }

    return true;
}

void GamescopeInstance::stop()
{
    if (!m_process) {
        return;
    }

    if (m_process->state() != QProcess::NotRunning) {
        m_status = QStringLiteral("Stopping...");
        Q_EMIT statusChanged();

        m_process->terminate();

        if (!m_process->waitForFinished(5000)) {
            m_process->kill();
            m_process->waitForFinished(2000);
        }
    }

    m_process->deleteLater();
    m_process = nullptr;

    m_status = QStringLiteral("Stopped");
    Q_EMIT statusChanged();
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

bool GamescopeInstance::isRunning() const
{
    return m_process && m_process->state() == QProcess::Running;
}

QStringList GamescopeInstance::buildGamescopeArgs(const QVariantMap &config)
{
    QStringList args;

    // Steam integration
    args << QStringLiteral("-e");

    // Borderless window
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

    // Scaling mode
    QString scalingMode = config.value(QStringLiteral("scalingMode"), QStringLiteral("fit")).toString();
    if (!scalingMode.isEmpty() && scalingMode != QStringLiteral("auto")) {
        args << QStringLiteral("-S") << scalingMode;
    }

    // Filter mode
    QString filterMode = config.value(QStringLiteral("filterMode"), QStringLiteral("linear")).toString();
    if (!filterMode.isEmpty()) {
        args << QStringLiteral("-F") << filterMode;
    }

    // Monitor selection
    int monitor = config.value(QStringLiteral("monitor"), 0).toInt();
    if (monitor > 0) {
        args << QStringLiteral("--display-index") << QString::number(monitor);
    }

    return args;
}

QString GamescopeInstance::buildSecondaryUserCommand(const QString &username,
                                                       const QStringList &gamescopeArgs,
                                                       const QString &steamArgs)
{
    // Build environment for PipeWire audio sharing
    QString env = QStringLiteral("PULSE_SERVER=tcp:127.0.0.1:4713");

    // Use sudo with run-as or direct sudo su
    // Format: sudo -u <username> env <env vars> gamescope <args> -- steam <steam args>
    QString command;

    // Check if run-as is available (preferred method)
    if (QFile::exists(QStringLiteral("/usr/bin/run-as"))) {
        command = QStringLiteral("sudo /usr/bin/run-as -X %1 -- env %2 /usr/bin/gamescope %3 -- /usr/bin/steam %4")
                      .arg(username, env, gamescopeArgs.join(QLatin1Char(' ')), steamArgs);
    } else {
        // Fallback to sudo -u
        command = QStringLiteral("sudo -u %1 env %2 DISPLAY=$DISPLAY /usr/bin/gamescope %3 -- /usr/bin/steam %4")
                      .arg(username, env, gamescopeArgs.join(QLatin1Char(' ')), steamArgs);
    }

    return command;
}

void GamescopeInstance::onProcessStarted()
{
    m_status = QStringLiteral("Running");
    Q_EMIT statusChanged();
    Q_EMIT runningChanged();
    Q_EMIT started();
}

void GamescopeInstance::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus)

    m_status = QStringLiteral("Exited (code %1)").arg(exitCode);
    Q_EMIT statusChanged();
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

void GamescopeInstance::onProcessError(QProcess::ProcessError error)
{
    QString errorMsg;
    switch (error) {
    case QProcess::FailedToStart:
        errorMsg = QStringLiteral("Failed to start gamescope");
        break;
    case QProcess::Crashed:
        errorMsg = QStringLiteral("Gamescope crashed");
        break;
    case QProcess::Timedout:
        errorMsg = QStringLiteral("Gamescope timed out");
        break;
    case QProcess::WriteError:
        errorMsg = QStringLiteral("Write error");
        break;
    case QProcess::ReadError:
        errorMsg = QStringLiteral("Read error");
        break;
    default:
        errorMsg = QStringLiteral("Unknown error");
    }

    m_status = errorMsg;
    Q_EMIT statusChanged();
    Q_EMIT errorOccurred(errorMsg);
}

void GamescopeInstance::onReadyReadStandardOutput()
{
    QString output = QString::fromUtf8(m_process->readAllStandardOutput());
    Q_EMIT outputReceived(output);
}

void GamescopeInstance::onReadyReadStandardError()
{
    QString output = QString::fromUtf8(m_process->readAllStandardError());
    Q_EMIT outputReceived(output);
}
