// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "StreamManager.h"

#include "HostProcess.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QRegularExpression>

namespace
{
constexpr int kLogPollIntervalMs = 250;
constexpr int kMaxRecentLines = 3;
constexpr int kTerminateWaitMs = 2000;

const QString kReadyMarker = QStringLiteral("Configuration UI available at [https://");
const QString kNoEncoderMarker = QStringLiteral("Unable to find display or encoder during startup.");

const QRegularExpression &gamepadPattern()
{
    static const QRegularExpression pattern(QStringLiteral("Gamepad (\\d+) will be"));
    return pattern;
}

const QRegularExpression &rejectionPattern()
{
    static const QRegularExpression pattern(QStringLiteral("\\b(invalid|unknown|not)\\b"));
    return pattern;
}

// Runs `<program> <conf> --creds couchplay <secret>`, which writes the credentials file named in the conf.
bool runCredentials(const QString &program, const QString &confPath, const QString &secret, QString *error)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    HostProcess::configure(process, program, SunshineConfig::credentialArguments(confPath, secret));
    process.start();
    if (!process.waitForStarted(5000)) {
        *error = process.errorString();
        return false;
    }
    if (!process.waitForFinished(StreamManager::CREDENTIALS_TIMEOUT_MS)) {
        process.kill();
        process.waitForFinished(1000);
        *error = QStringLiteral("timed out");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString output = QString::fromUtf8(process.readAll()).trimmed();
        *error = output.isEmpty() ? QStringLiteral("exit code %1").arg(process.exitCode()) : output.right(200);
        return false;
    }
    return true;
}
}

StreamManager::StreamManager(QObject *parent)
    : QObject(parent)
{
    m_logTimer.setInterval(kLogPollIntervalMs);
    connect(&m_logTimer, &QTimer::timeout, this, &StreamManager::pollLogs);
}

StreamManager::~StreamManager()
{
    for (Stream *stream : std::as_const(m_streams)) {
        if (QProcess *process = stream->process) {
            process->disconnect(this);
            process->terminate();
            if (!process->waitForFinished(kTerminateWaitMs)) {
                process->kill();
                process->waitForFinished(500);
            }
            delete process;
        }
        delete stream;
    }
    m_streams.clear();
}

QVariantList StreamManager::streams() const
{
    QVariantList result;
    for (auto it = m_streams.constBegin(); it != m_streams.constEnd(); ++it) {
        const Stream &stream = *it.value();
        QVariantMap entry;
        entry[QStringLiteral("instanceIndex")] = it.key();
        entry[QStringLiteral("state")] = static_cast<int>(stream.state);
        entry[QStringLiteral("port")] = stream.spec.port;
        entry[QStringLiteral("sunshineName")] = SunshineConfig::displayName(stream.spec);
        entry[QStringLiteral("outputName")] = stream.spec.outputName;
        entry[QStringLiteral("error")] = stream.error;
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

void StreamManager::setSunshinePath(const QString &path)
{
    m_sunshinePath = path;
}

void StreamManager::setProgramOverride(const QString &program)
{
    m_programOverride = program;
}

void StreamManager::setRestartDelay(int delayMs)
{
    m_restartDelay = delayMs;
}

StreamManager::Stream *StreamManager::createStream(int instanceIndex, const StreamSpec &spec)
{
    auto *stream = new Stream;
    stream->spec = spec;

    stream->startupTimer = new QTimer(this);
    stream->startupTimer->setSingleShot(true);
    connect(stream->startupTimer, &QTimer::timeout, this, [this, instanceIndex]() {
        onStartupTimeout(instanceIndex);
    });

    stream->killTimer = new QTimer(this);
    stream->killTimer->setSingleShot(true);
    connect(stream->killTimer, &QTimer::timeout, this, [this, instanceIndex]() {
        const Stream *current = m_streams.value(instanceIndex, nullptr);
        if (current && current->process) {
            current->process->kill();
        }
    });

    stream->restartTimer = new QTimer(this);
    stream->restartTimer->setSingleShot(true);
    connect(stream->restartTimer, &QTimer::timeout, this, [this, instanceIndex]() {
        launch(instanceIndex);
    });

    m_streams.insert(instanceIndex, stream);
    return stream;
}

void StreamManager::removeStream(int instanceIndex)
{
    Stream *stream = m_streams.take(instanceIndex);
    if (!stream) {
        return;
    }
    for (QTimer *timer : {stream->startupTimer, stream->killTimer, stream->restartTimer}) {
        timer->stop();
        timer->deleteLater();
    }
    delete stream;
}

bool StreamManager::failStart(int instanceIndex, const StreamSpec &spec, const QString &message)
{
    qWarning() << "StreamManager: stream" << instanceIndex << "failed to start:" << message;
    removeStream(instanceIndex);
    Stream *stream = createStream(instanceIndex, spec);
    stream->error = message;
    setState(*stream, Error);
    Q_EMIT streamsChanged();
    Q_EMIT streamError(instanceIndex, message);
    return false;
}

bool StreamManager::startStream(int instanceIndex, const StreamSpec &spec)
{
    if (instanceIndex < 0) {
        qWarning() << "StreamManager::startStream: invalid instanceIndex" << instanceIndex;
        return false;
    }

    if (const Stream *existing = m_streams.value(instanceIndex, nullptr)) {
        if (existing->process || existing->restartTimer->isActive()) {
            qWarning() << "StreamManager::startStream: instance" << instanceIndex << "already active";
            Q_EMIT streamError(instanceIndex, QStringLiteral("Instance already active"));
            return false;
        }
        // Left over from an earlier failure
        removeStream(instanceIndex);
    }

    const QString program = m_programOverride.isEmpty() ? m_sunshinePath : m_programOverride;
    if (program.isEmpty()) {
        return failStart(instanceIndex,
                         spec,
                         QStringLiteral("Sunshine is not installed; set its path in Settings"));
    }

    const QString stateDir = SunshineConfig::ensureStateDirectory(spec.identityKey);
    if (stateDir.isEmpty()) {
        return failStart(instanceIndex, spec, QStringLiteral("Could not create the Sunshine state directory"));
    }

    const QString confPath = SunshineConfig::generateConfig(spec, stateDir);
    if (confPath.isEmpty()) {
        return failStart(instanceIndex, spec, QStringLiteral("Could not write the Sunshine configuration"));
    }

    const QString secret = SunshineConfig::secret(stateDir);
    if (secret.isEmpty()) {
        return failStart(instanceIndex, spec, QStringLiteral("Could not create the Sunshine credentials secret"));
    }

    if (!QFile::exists(stateDir + QStringLiteral("/credentials.json"))) {
        QString credentialsError;
        if (!runCredentials(program, confPath, secret, &credentialsError)) {
            return failStart(instanceIndex,
                             spec,
                             QStringLiteral("Could not set up Sunshine credentials: %1").arg(credentialsError));
        }
    }

    Stream *stream = createStream(instanceIndex, spec);
    stream->stateDir = stateDir;
    stream->confPath = confPath;
    stream->logPath = stateDir + QStringLiteral("/sunshine.log");
    stream->program = program;
    launch(instanceIndex);

    const Stream *current = m_streams.value(instanceIndex, nullptr);
    return current && current->state != Error;
}

void StreamManager::launch(int instanceIndex)
{
    Stream *stream = m_streams.value(instanceIndex, nullptr);
    if (!stream) {
        return;
    }

    // Start from an empty log so stale lines from an earlier run are never scanned
    QFile log(stream->logPath);
    if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "StreamManager: could not truncate" << stream->logPath << log.errorString();
    }
    log.close();

    stream->logOffset = 0;
    stream->logBuffer.clear();
    stream->recentLines.clear();
    stream->launchError.clear();
    stream->error.clear();
    stream->ready = false;
    stream->failed = false;

    auto *process = new QProcess(this);
    process->setStandardOutputFile(QProcess::nullDevice());
    process->setStandardErrorFile(QProcess::nullDevice());
    HostProcess::configure(*process, stream->program, {stream->confPath});

    connect(process, &QProcess::finished, this, [this, instanceIndex, process](int exitCode, QProcess::ExitStatus) {
        onProcessFinished(instanceIndex, process, exitCode);
    });
    connect(process, &QProcess::errorOccurred, this, [this, instanceIndex, process](QProcess::ProcessError error) {
        // A process that never started emits no finished()
        if (error != QProcess::FailedToStart) {
            return;
        }
        Stream *current = m_streams.value(instanceIndex, nullptr);
        if (current && current->process == process) {
            current->launchError = process->errorString();
            onProcessFinished(instanceIndex, process, -1);
        }
    });

    stream->process = process;
    setState(*stream, Waiting);
    stream->startupTimer->start(m_startupTimeout);
    m_logTimer.start();
    process->start();
}

void StreamManager::setState(Stream &stream, StreamState state)
{
    if (stream.state == state) {
        return;
    }
    stream.state = state;
    Q_EMIT streamsChanged();
}

void StreamManager::markFailed(int instanceIndex, const QString &message)
{
    Stream *stream = m_streams.value(instanceIndex, nullptr);
    if (!stream || stream->failed || stream->stopping) {
        return;
    }
    qWarning() << "StreamManager: stream" << instanceIndex << "failed:" << message;
    stream->failed = true;
    stream->error = message;
    stream->startupTimer->stop();
    setState(*stream, Error);
    Q_EMIT streamsChanged();
    if (stream->process) {
        stream->process->kill();
    }
    Q_EMIT streamError(instanceIndex, message);
}

void StreamManager::onStartupTimeout(int instanceIndex)
{
    const Stream *stream = m_streams.value(instanceIndex, nullptr);
    if (!stream || !stream->process || stream->ready || stream->failed || stream->stopping) {
        return;
    }
    markFailed(instanceIndex,
               QStringLiteral("Sunshine did not become ready within %1 s.%2")
                   .arg(m_startupTimeout / 1000)
                   .arg(recentLogSummary(*stream)));
}

void StreamManager::onProcessFinished(int instanceIndex, QProcess *process, int exitCode)
{
    const Stream *check = m_streams.value(instanceIndex, nullptr);
    if (!check || check->process != process) {
        return;
    }

    // Pick up lines written just before exit (fatal startup errors in particular)
    readLog(instanceIndex);

    Stream *stream = m_streams.value(instanceIndex, nullptr);
    if (!stream || stream->process != process) {
        return;
    }

    stream->startupTimer->stop();
    stream->killTimer->stop();
    stream->process = nullptr;
    process->deleteLater();

    if (stream->stopping) {
        removeStream(instanceIndex);
        updateLogTimer();
        Q_EMIT streamsChanged();
        Q_EMIT streamStopped(instanceIndex);
        return;
    }

    if (stream->failed) {
        // streamError was already emitted; keep the entry so the error stays visible
        updateLogTimer();
        Q_EMIT streamStopped(instanceIndex);
        return;
    }

    if (!stream->wasReady) {
        const QString reason = stream->launchError.isEmpty()
            ? QStringLiteral("Sunshine exited before it was ready (exit code %1).%2")
                  .arg(exitCode)
                  .arg(recentLogSummary(*stream))
            : QStringLiteral("Could not start Sunshine: %1").arg(stream->launchError);
        stream->error = reason;
        setState(*stream, Error);
        Q_EMIT streamsChanged();
        updateLogTimer();
        qWarning() << "StreamManager: stream" << instanceIndex << reason;
        Q_EMIT streamError(instanceIndex, reason);
        Q_EMIT streamStopped(instanceIndex);
        return;
    }

    // Unexpected exit after the stream was ready: bounded restart
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    stream->restartTimes.removeIf([now](qint64 time) {
        return now - time > RESTART_WINDOW_MS;
    });
    stream->ready = false;

    if (m_autoRestart && stream->restartTimes.size() < MAX_RESTARTS) {
        stream->restartTimes.append(now);
        qWarning() << "StreamManager: Sunshine for stream" << instanceIndex << "exited unexpectedly; restart"
                   << stream->restartTimes.size() << "of" << MAX_RESTARTS;
        setState(*stream, Waiting);
        updateLogTimer();
        stream->restartTimer->start(m_restartDelay);
        return;
    }

    const QString reason = m_autoRestart
        ? QStringLiteral("Sunshine kept exiting unexpectedly and was restarted %1 times").arg(MAX_RESTARTS)
        : QStringLiteral("Sunshine exited unexpectedly (exit code %1)").arg(exitCode);
    stream->error = reason;
    setState(*stream, Error);
    Q_EMIT streamsChanged();
    updateLogTimer();
    Q_EMIT streamError(instanceIndex, reason);
    Q_EMIT streamStopped(instanceIndex);
}

void StreamManager::pollLogs()
{
    const QList<int> keys = m_streams.keys();
    for (const int instanceIndex : keys) {
        const Stream *stream = m_streams.value(instanceIndex, nullptr);
        if (stream && stream->process) {
            readLog(instanceIndex);
        }
    }
}

void StreamManager::readLog(int instanceIndex)
{
    Stream *stream = m_streams.value(instanceIndex, nullptr);
    if (!stream) {
        return;
    }

    QFile file(stream->logPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    if (file.size() < stream->logOffset) {
        // Truncated behind our back
        stream->logOffset = 0;
        stream->logBuffer.clear();
    }
    file.seek(stream->logOffset);
    const QByteArray data = file.readAll();
    file.close();
    if (data.isEmpty()) {
        return;
    }
    stream->logOffset += data.size();
    stream->logBuffer.append(data);

    for (;;) {
        const qsizetype newline = stream->logBuffer.indexOf('\n');
        if (newline < 0) {
            break;
        }
        const QString line = QString::fromUtf8(stream->logBuffer.left(newline)).trimmed();
        stream->logBuffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }
        processLine(instanceIndex, line);
        // A signal handler may have stopped and removed the stream
        if (m_streams.value(instanceIndex, nullptr) != stream) {
            return;
        }
    }
}

void StreamManager::processLine(int instanceIndex, const QString &line)
{
    Stream *stream = m_streams.value(instanceIndex, nullptr);
    if (!stream) {
        return;
    }

    stream->recentLines.append(line);
    while (stream->recentLines.size() > kMaxRecentLines) {
        stream->recentLines.removeFirst();
    }

    if (gamepadPattern().match(line).hasMatch()) {
        if (!stream->failed && !stream->stopping) {
            Q_EMIT gamepadAnnounced(instanceIndex);
        }
        return;
    }

    if (stream->failed || stream->stopping || stream->ready) {
        return;
    }

    if (line.contains(kNoEncoderMarker)) {
        markFailed(instanceIndex,
                   QStringLiteral("Sunshine could not capture %1 or found no encoder").arg(stream->spec.outputName));
        return;
    }

    const QString lower = line.toLower();
    if (lower.contains(QLatin1String("capture")) && lower.contains(QLatin1String("kwin"))
        && rejectionPattern().match(lower).hasMatch()) {
        markFailed(instanceIndex,
                   QStringLiteral("Sunshine at %1 does not support KWin capture; install Sunshine 2026.516 or newer")
                       .arg(stream->program));
        return;
    }

    if (line.contains(kReadyMarker)) {
        stream->ready = true;
        stream->wasReady = true;
        stream->startupTimer->stop();
        setState(*stream, Streaming);
        Q_EMIT streamStarted(instanceIndex);
    }
}

QString StreamManager::recentLogSummary(const Stream &stream) const
{
    if (stream.recentLines.isEmpty()) {
        return {};
    }
    return QStringLiteral(" Last log lines: %1").arg(stream.recentLines.join(QStringLiteral(" | ")));
}

void StreamManager::updateLogTimer()
{
    for (const Stream *stream : std::as_const(m_streams)) {
        if (stream->process) {
            m_logTimer.start();
            return;
        }
    }
    m_logTimer.stop();
}

bool StreamManager::stopStream(int instanceIndex)
{
    Stream *stream = m_streams.value(instanceIndex, nullptr);
    if (!stream) {
        return false;
    }

    if (stream->stopping) {
        return stream->process != nullptr;
    }

    if (stream->process) {
        stream->stopping = true;
        stream->restartTimer->stop();
        stream->startupTimer->stop();
        stream->process->terminate();
        stream->killTimer->start(STOP_KILL_DELAY_MS);
        return true;
    }

    const bool restartPending = stream->restartTimer->isActive();
    removeStream(instanceIndex);
    updateLogTimer();
    Q_EMIT streamsChanged();
    if (restartPending) {
        Q_EMIT streamStopped(instanceIndex);
    }
    return restartPending;
}

void StreamManager::stopAll()
{
    const QList<int> keys = m_streams.keys();
    for (const int instanceIndex : keys) {
        stopStream(instanceIndex);
    }
}

StreamManager::StreamState StreamManager::streamState(int instanceIndex) const
{
    const Stream *stream = m_streams.value(instanceIndex, nullptr);
    return stream ? stream->state : NotStarted;
}

bool StreamManager::isStreaming(int instanceIndex) const
{
    return streamState(instanceIndex) == Streaming;
}

bool StreamManager::hasOwnedPids() const
{
    for (const Stream *stream : std::as_const(m_streams)) {
        if (stream->process) {
            return true;
        }
    }
    return false;
}

bool StreamManager::hasOwnedPid(int instanceIndex) const
{
    const Stream *stream = m_streams.value(instanceIndex, nullptr);
    return stream && stream->process;
}
