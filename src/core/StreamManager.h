// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include "SunshineConfig.h"

#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <qqmlintegration.h>

class SunshineApiClient;

/**
 * @brief Runs one Sunshine process per streaming player, as the desktop user on the host
 *
 * Sunshine captures a single KWin output (`capture = kwin`). It is started through HostProcess
 * (flatpak-spawn --host under Flatpak) because KWin denies screen capture to sandboxed clients.
 * Readiness and failures are detected by tailing Sunshine's log file.
 *
 * Before Ready any failure is a startup failure (streamError, no restart). After Ready an
 * unexpected exit restarts Sunshine up to MAX_RESTARTS times within RESTART_WINDOW_MS.
 */
class StreamManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList streams READ streams NOTIFY streamsChanged)
    Q_PROPERTY(QString hostAddress READ hostAddress CONSTANT)
    Q_PROPERTY(bool autoRestart READ autoRestart WRITE setAutoRestart NOTIFY autoRestartChanged)
    Q_PROPERTY(int startupTimeout READ startupTimeout WRITE setStartupTimeout NOTIFY startupTimeoutChanged)

public:
    explicit StreamManager(QObject *parent = nullptr);
    ~StreamManager() override;

    enum StreamState {
        NotStarted = 0,
        Waiting,
        Streaming,
        Error
    };
    Q_ENUM(StreamState)

    static constexpr int MAX_RESTARTS = 3;
    static constexpr int RESTART_WINDOW_MS = 60000;
    static constexpr int STOP_KILL_DELAY_MS = 5000;
    static constexpr int CREDENTIALS_TIMEOUT_MS = 15000;

    /**
     * Entries: {instanceIndex, state, port, sunshineName, outputName, error, pendingPairings, clients, legacyPairing}.
     * The last three are filled by refreshPairing(): pendingPairings = [{id, name, address}],
     * clients = [{name, uuid}].
     */
    QVariantList streams() const;

    /** First non-loopback IPv4 address of this machine; empty when there is none. */
    QString hostAddress() const;

    bool autoRestart() const;
    void setAutoRestart(bool enabled);

    int startupTimeout() const;
    void setStartupTimeout(int timeoutMs);

    /** Sunshine executable (SettingsManager::resolvedSunshinePath()); empty if not installed. */
    void setSunshinePath(const QString &path);

    /** Test seam: run @p program instead of the Sunshine path (for --creds and the main launch). */
    void setProgramOverride(const QString &program);

    /** Test seam: delay before an automatic restart (default 2000 ms). */
    void setRestartDelay(int delayMs);

    /**
     * Prepare the state directory, config and credentials, then start Sunshine.
     * Failures emit streamError() and return false. Readiness arrives via streamStarted().
     */
    bool startStream(int instanceIndex, const StreamSpec &spec);

    /** Request a stop. Returns true when a running (or restart-pending) stream is being stopped. */
    Q_INVOKABLE bool stopStream(int instanceIndex);
    Q_INVOKABLE void stopAll();

    /** Fetch pending pairings and paired clients from the stream's Sunshine (only while Streaming). */
    Q_INVOKABLE void refreshPairing(int instanceIndex);
    /** Submit the PIN shown by a Moonlight client; the outcome arrives as pinResult(). */
    Q_INVOKABLE void submitPin(int instanceIndex, const QString &pairingId, const QString &pin, const QString &clientName);
    Q_INVOKABLE void unpairClient(int instanceIndex, const QString &uuid);
    /** While active, refreshPairing() runs every 2 s for this stream (until it stops). */
    Q_INVOKABLE void setPairingActive(int instanceIndex, bool active);

    StreamState streamState(int instanceIndex) const;
    bool isStreaming(int instanceIndex) const;
    bool hasOwnedPids() const;
    bool hasOwnedPid(int instanceIndex) const;

Q_SIGNALS:
    /** Sunshine reported ready (Waiting -> Streaming). */
    void streamStarted(int instanceIndex);
    /** The Sunshine process is gone and the stream is cleaned up (stop, crash without restart, failure). */
    void streamStopped(int instanceIndex);
    void streamError(int instanceIndex, const QString &error);
    /** Sunshine is about to create a virtual gamepad for the stream (log line "Gamepad N will be ..."). */
    void gamepadAnnounced(int instanceIndex);
    void pinResult(int instanceIndex, bool ok, const QString &message);
    /** Outcome of unpairClient(); message is empty on success */
    void unpairResult(int instanceIndex, bool ok, const QString &message);
    void streamsChanged();
    void autoRestartChanged();
    void startupTimeoutChanged();

private:
    struct Stream {
        StreamSpec spec;
        StreamState state = NotStarted;
        QString stateDir;
        QString confPath;
        QString logPath;
        QString program;
        QString error;
        QString launchError;
        QPointer<QProcess> process;
        QTimer *startupTimer = nullptr;
        QTimer *killTimer = nullptr;
        QTimer *restartTimer = nullptr;
        QTimer *pairingTimer = nullptr;
        SunshineApiClient *api = nullptr;
        QVariantList pendingPairings;
        QVariantList clients;
        bool legacyPairing = false;
        bool refreshing = false; ///< a pairing refresh request is in flight
        bool pinPending = false; ///< a PIN submission is in flight
        bool unpairPending = false; ///< an unpair request is in flight
        qint64 logOffset = 0;
        QByteArray logBuffer;
        QStringList recentLines;
        QList<qint64> restartTimes;
        bool ready = false; ///< current process reported ready
        bool wasReady = false; ///< any process of this stream reported ready
        bool stopping = false; ///< a stop was requested
        bool failed = false; ///< startup failure reported, process being killed
    };

    Stream *createStream(int instanceIndex, const StreamSpec &spec);
    bool failStart(int instanceIndex, const StreamSpec &spec, const QString &message);
    void launch(int instanceIndex);
    void setState(Stream &stream, StreamState state);
    void markFailed(int instanceIndex, const QString &message);
    void onProcessFinished(int instanceIndex, QProcess *process, int exitCode);
    void onStartupTimeout(int instanceIndex);
    void pollLogs();
    void readLog(int instanceIndex);
    void processLine(int instanceIndex, const QString &line);
    void removeStream(int instanceIndex);
    void updateLogTimer();
    SunshineApiClient *apiClient(int instanceIndex);
    void onPairingsFetched(int instanceIndex, const QVariantList &pairings, bool legacy);
    void onClientsFetched(int instanceIndex, const QVariantList &clients);
    void onApiFailed(int instanceIndex, const QString &message);
    QString recentLogSummary(const Stream &stream) const;

    QMap<int, Stream *> m_streams;
    QTimer m_logTimer;
    QString m_sunshinePath;
    QString m_programOverride;
    bool m_autoRestart = true;
    int m_startupTimeout = 20000;
    int m_restartDelay = 2000;
};
