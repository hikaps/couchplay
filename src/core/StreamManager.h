// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QMap>
#include <QTimer>
#include <QPointer>
#include <qqmlintegration.h>

struct InstanceConfig;

/**
 * @brief Manages Sunshine streaming subprocesses per player instance
 *
 * Each streaming player gets one Sunshine process launched via the D-Bus helper.
 * Config directories are created per-instance, cleaned up on stop.
 * Supports auto-restart on crash, port conflict retry, and startup timeout.
 */
class StreamManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList streams READ streams NOTIFY streamsChanged)
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

    QVariantList streams() const;

    bool autoRestart() const;
    void setAutoRestart(bool enabled);

    int startupTimeout() const;
    void setStartupTimeout(int timeoutMs);

    Q_INVOKABLE bool startStream(int instanceIndex, const QVariantMap &config);
    Q_INVOKABLE bool stopStream(int instanceIndex);
    Q_INVOKABLE void stopAll();

    StreamState streamState(int instanceIndex) const;
    bool isStreaming(int instanceIndex) const;

Q_SIGNALS:
    void streamStarted(int instanceIndex);
    void streamStopped(int instanceIndex);
    void streamError(int instanceIndex, const QString &error);
    void streamsChanged();
    void autoRestartChanged();
    void startupTimeoutChanged();

private Q_SLOTS:
    void onHelperInstanceStopped(const QString &username, qint64 pid, const QString &reason);
    void onStartupTimeout();

private:
    void setStreamState(int instanceIndex, StreamState state);
    void cleanupConfigDir(int instanceIndex);
    void attemptRestart(int instanceIndex);

    struct StreamEntry {
        StreamState state = NotStarted;
        qint64 pid = 0;
        QString configDir;
        QString username;
        int instanceIndex = -1;
        QVariantMap lastConfig;
        int restartAttempts = 0;
        static constexpr int MAX_RESTART_ATTEMPTS = 3;
    };

    QMap<int, StreamEntry> m_streams;
    QMap<int, QTimer *> m_startupTimers;
    QMap<int, QPointer<QTimer>> m_restartTimers;
    bool m_autoRestart = true;
    int m_startupTimeout = 15000;
};
