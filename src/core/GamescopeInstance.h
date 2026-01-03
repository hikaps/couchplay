// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QList>
#include <qqmlintegration.h>

struct InstanceConfig;

/**
 * @brief Manages a single gamescope instance and its child Steam process
 */
class GamescopeInstance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int index READ index CONSTANT)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(qint64 pid READ pid NOTIFY runningChanged)

public:
    explicit GamescopeInstance(QObject *parent = nullptr);
    ~GamescopeInstance() override;

    /**
     * @brief Start the gamescope instance
     * @param config Instance configuration
     * @param index Instance index (0 = primary, 1+ = secondary)
     * @return true if started successfully
     */
    Q_INVOKABLE bool start(const QVariantMap &config, int index);

    /**
     * @brief Stop the gamescope instance
     */
    Q_INVOKABLE void stop();

    /**
     * @brief Check if instance is running
     */
    bool isRunning() const;

    int index() const { return m_index; }
    qint64 pid() const { return m_process ? m_process->processId() : 0; }
    QString status() const { return m_status; }

    /**
     * @brief Build gamescope command line arguments
     */
    static QStringList buildGamescopeArgs(const QVariantMap &config);

    /**
     * @brief Build the full command for secondary user (using run-as or sudo)
     */
    static QString buildSecondaryUserCommand(const QString &username, 
                                              const QStringList &gamescopeArgs,
                                              const QString &steamArgs);

Q_SIGNALS:
    void runningChanged();
    void statusChanged();
    void started();
    void stopped();
    void errorOccurred(const QString &message);
    void outputReceived(const QString &output);

private Q_SLOTS:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onReadyReadStandardOutput();
    void onReadyReadStandardError();

private:
    QProcess *m_process = nullptr;
    int m_index = -1;
    QString m_status;
    bool m_isPrimary = true;
};
