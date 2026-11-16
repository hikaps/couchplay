// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "GamescopeInstance.h"
#include "HostProcess.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QUuid>

#include <csignal>
#include <unistd.h>

namespace
{
constexpr int kPollIntervalMs = 250;
constexpr qint64 kStartTimeoutMs = 20000;
constexpr qsizetype kStderrTailBytes = 4096;

// Prints the host PID of the gamescope process whose environment carries our sandbox token.
const char *kScanScript = "for d in /proc/[0-9]*; do "
                          "case \"$(cat \"$d/comm\" 2>/dev/null)\" in gamescope|gamescope-wl) ;; *) continue;; esac; "
                          "if tr '\\0' '\\n' < \"$d/environ\" 2>/dev/null | grep -qx \"COUCHPLAY_SANDBOX_ID=$1\"; then "
                          "echo \"${d#/proc/}\"; exit 0; fi; done; exit 1";

QString lastLines(const QByteArray &text, int count)
{
    const QStringList lines = QString::fromLocal8Bit(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.mid(qMax<qsizetype>(0, lines.size() - count)).join(QLatin1Char('\n'));
}

// Ubuntu 23.10+ blocks unprivileged user namespaces for programs without an AppArmor profile; bwrap then fails
// while mapping its uid, or when creating the namespace.
QString userNamespaceHint(const QString &detail)
{
    if (!detail.contains(QLatin1String("setting up uid map")) && !detail.contains(QLatin1String("creating new namespace"))
        && !detail.contains(QLatin1String("No permissions to create"))) {
        return {};
    }
    return QStringLiteral("\n\nYour system blocks user namespaces for bwrap (typical on Ubuntu 24.04 and newer). "
                          "See the \"Ubuntu: bwrap permission denied\" section of the CouchPlay README.");
}
}

GamescopeInstance::GamescopeInstance(QObject *parent)
    : QObject(parent)
{
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyRead, this, [this]() {
        m_stderrTail += m_process.readAll();
        if (m_stderrTail.size() > kStderrTailBytes) {
            m_stderrTail = m_stderrTail.right(kStderrTailBytes);
        }
    });
    connect(&m_process, &QProcess::finished, this, &GamescopeInstance::onProcessFinished);
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &GamescopeInstance::pollForGamescope);
}

GamescopeInstance::~GamescopeInstance()
{
    // The owner may already be tearing down; don't signal into it.
    disconnect(this, nullptr, nullptr, nullptr);
    if (m_launched) {
        kill();
    }
}

bool GamescopeInstance::start(const QVariantMap &config, int index, const QList<SandboxMount> &mounts)
{
    if (m_launched) {
        Q_EMIT errorOccurred(QStringLiteral("Instance already running"));
        return false;
    }

    m_index = index;
    m_player = config.value(QStringLiteral("player")).toString();
    const int posX = config.value(QStringLiteral("positionX"), 0).toInt();
    const int posY = config.value(QStringLiteral("positionY"), 0).toInt();
    const int outputW = config.value(QStringLiteral("outputWidth"), 960).toInt();
    const int outputH = config.value(QStringLiteral("outputHeight"), 1080).toInt();
    m_windowGeometry = QRect(posX, posY, outputW, outputH);
    Q_EMIT configChanged();

    const QStringList gameCommand = config.value(QStringLiteral("gameCommand")).toStringList();
    if (gameCommand.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Launch command is empty"));
        return false;
    }

    SandboxSpec spec;
    spec.home = QDir::homePath();
    spec.playerHome = m_player.isEmpty() ? QString() : config.value(QStringLiteral("playerHome")).toString();
    spec.runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    spec.padNode = config.value(QStringLiteral("padNode")).toString();
    spec.directDevices = config.value(QStringLiteral("directDevices")).toStringList();
    const QStringList nvidiaNodes =
        QDir(QStringLiteral("/dev")).entryList({QStringLiteral("nvidia*")}, QDir::System | QDir::Files);
    for (const QString &node : nvidiaNodes) {
        spec.extraDeviceNodes << QStringLiteral("/dev/") + node;
    }
    spec.mounts = mounts;
    for (const QString &entry : buildEnvironment(config)) {
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        if (equals > 0) {
            spec.env.insert(entry.left(equals), entry.mid(equals + 1));
        }
    }
    m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    spec.token = m_token;
    spec.workingDirectory = config.value(QStringLiteral("workingDirectory")).toString();
    spec.gamescopeArgs = buildGamescopeArgs(config);
    spec.gameCommand = gameCommand;
    spec.uid = ::getuid();
    spec.gid = ::getgid();

    const QStringList argv = buildSandboxArgv(spec);
    m_stderrTail.clear();
    m_gamescopePid = 0;
    HostProcess::configure(m_process, argv.first(), argv.mid(1));
    m_process.start();
    if (!m_process.waitForStarted(5000)) {
        Q_EMIT errorOccurred(QStringLiteral("Could not start the sandbox: %1").arg(m_process.errorString()));
        return false;
    }

    m_launched = true;
    m_startDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kStartTimeoutMs;
    setStatus(QStringLiteral("Starting"));
    m_pollTimer.start();
    Q_EMIT runningChanged();
    return true;
}

void GamescopeInstance::pollForGamescope()
{
    if (!m_launched || m_gamescopePid > 0) {
        m_pollTimer.stop();
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() > m_startDeadlineMs) {
        const QString detail = lastLines(m_stderrTail, 20);
        m_stopping = true; // so onProcessFinished leaves the timeout report to this branch
        m_process.kill();
        m_process.waitForFinished(2000);
        if (!m_launched) {
            return; // onProcessFinished already reported the failure
        }
        resetLaunchState();
        setStatus(QStringLiteral("Failed"));
        Q_EMIT runningChanged();
        Q_EMIT errorOccurred(detail.isEmpty() ? QStringLiteral("Gamescope did not start inside the sandbox")
                                              : QStringLiteral("Gamescope did not start inside the sandbox:\n") + detail);
        Q_EMIT stopped();
        return;
    }
    if (m_scan && m_scan->state() != QProcess::NotRunning) {
        return;
    }

    if (m_scan) {
        m_scan->disconnect(this);
        m_scan.release()->deleteLater();
    }
    m_scan = std::make_unique<QProcess>();
    HostProcess::configure(*m_scan,
                           QStringLiteral("sh"),
                           {QStringLiteral("-c"), QString::fromLatin1(kScanScript), QStringLiteral("sh"), m_token});
    QProcess *scan = m_scan.get();
    connect(scan, &QProcess::finished, this, [this, scan](int exitCode, QProcess::ExitStatus status) {
        if (scan != m_scan.get() || !m_launched || m_gamescopePid > 0) {
            return;
        }
        if (status != QProcess::NormalExit || exitCode != 0) {
            return;
        }
        const qint64 pid = QString::fromLocal8Bit(scan->readAllStandardOutput()).trimmed().toLongLong();
        if (pid <= 0) {
            return;
        }
        m_gamescopePid = pid;
        m_pollTimer.stop();
        setStatus(m_player.isEmpty() ? QStringLiteral("Running") : QStringLiteral("Running as %1").arg(m_player));
        Q_EMIT gamescopePidChanged();
        Q_EMIT runningChanged();
        Q_EMIT started();
    });
    scan->start();
}

void GamescopeInstance::signalHost(const QString &signal)
{
    if (m_gamescopePid <= 0) {
        return;
    }
    if (!HostProcess::runningInFlatpak()) {
        ::kill(static_cast<pid_t>(m_gamescopePid), signal == QStringLiteral("KILL") ? SIGKILL : SIGTERM);
        return;
    }
    QProcess killer;
    HostProcess::configure(killer,
                           QStringLiteral("kill"),
                           {QStringLiteral("-") + signal, QString::number(m_gamescopePid)});
    killer.start();
    if (!killer.waitForFinished(3000)) {
        killer.kill();
        killer.waitForFinished(500);
    }
}

void GamescopeInstance::stop(int timeoutMs)
{
    if (!m_launched) {
        return;
    }
    setStatus(QStringLiteral("Stopping..."));
    m_pollTimer.stop();
    m_stopping = true;
    if (m_gamescopePid > 0) {
        signalHost(QStringLiteral("TERM"));
    } else {
        m_process.terminate();
    }
    if (!m_process.waitForFinished(timeoutMs)) {
        signalHost(QStringLiteral("KILL"));
        m_process.kill();
        m_process.waitForFinished(2000);
    }
    finishStop(QStringLiteral("Stopped"));
}

void GamescopeInstance::kill()
{
    if (!m_launched) {
        return;
    }
    setStatus(QStringLiteral("Killing..."));
    m_pollTimer.stop();
    m_stopping = true;
    signalHost(QStringLiteral("KILL"));
    m_process.kill();
    m_process.waitForFinished(2000);
    finishStop(QStringLiteral("Killed"));
}

void GamescopeInstance::finishStop(const QString &status)
{
    // waitForFinished() delivers finished() synchronously, so onProcessFinished may have reported it.
    if (!m_launched) {
        return;
    }
    resetLaunchState();
    setStatus(status);
    Q_EMIT gamescopePidChanged();
    Q_EMIT runningChanged();
    Q_EMIT stopped();
}

void GamescopeInstance::resetLaunchState()
{
    m_launched = false;
    m_stopping = false;
    m_gamescopePid = 0;
    m_pollTimer.stop();
    if (m_scan) {
        // May run inside the scan's own finished handler (started() -> stop()); delete after it returns.
        m_scan->disconnect(this);
        m_scan.release()->deleteLater();
    }
}

void GamescopeInstance::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode)
    if (!m_launched || m_stopping) {
        return; // an explicit stop()/kill() reports this itself
    }
    const bool wasRunning = m_gamescopePid > 0;
    const QString detail = lastLines(m_stderrTail, 20);
    resetLaunchState();

    if (!wasRunning) {
        setStatus(QStringLiteral("Failed"));
        Q_EMIT gamescopePidChanged();
        Q_EMIT runningChanged();
        QString message = detail.isEmpty() ? QStringLiteral("The sandbox exited before gamescope started")
                                           : QStringLiteral("The sandbox exited before gamescope started:\n") + detail;
        message += userNamespaceHint(detail);
        Q_EMIT errorOccurred(message);
        Q_EMIT stopped();
        return;
    }

    const bool crashed = exitStatus == QProcess::CrashExit;
    setStatus(crashed ? QStringLiteral("Crashed") : QStringLiteral("Exited unexpectedly"));
    Q_EMIT gamescopePidChanged();
    Q_EMIT runningChanged();
    Q_EMIT errorOccurred(crashed ? QStringLiteral("crashed") : QStringLiteral("exited"));
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

    // outputWidth/outputHeight are the logical window size; gamescope takes its window size in physical pixels
    const qreal outputScale = config.value(QStringLiteral("outputScale"), 1.0).toReal();
    const int outputW = config.value(QStringLiteral("outputWidth"), 960).toInt();
    const int outputH = config.value(QStringLiteral("outputHeight"), 1080).toInt();
    args << QStringLiteral("-W") << QString::number(qRound(outputW * outputScale));
    args << QStringLiteral("-H") << QString::number(qRound(outputH * outputScale));

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

    const QString monitorName = config.value(QStringLiteral("monitorName")).toString();
    if (!monitorName.isEmpty()) {
        args << QStringLiteral("--prefer-output") << monitorName;
    }

    // NOTE: Input device isolation is handled by the sandbox: its /dev only contains the player's virtual
    // pad (see couchplay-padfwd) and directly assigned input nodes. gamescope has no per-device flag.

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

    // Games started by this player's Steam must reach its private IPC port, not the desktop's (57343)
    const QString steamIpcAddress = config.value(QStringLiteral("steamIpcAddress")).toString();
    if (!steamIpcAddress.isEmpty()) {
        envVars << QStringLiteral("steam_master_ipc_name_override=%1").arg(steamIpcAddress);
    }

    return envVars;
}
