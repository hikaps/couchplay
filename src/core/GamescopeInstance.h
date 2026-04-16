// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QRect>
#include <QVariantMap>
#include <qqmlintegration.h>

struct InstanceConfig;

/**
 * @brief Manages a single gamescope instance
 */
class GamescopeInstance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int index READ index CONSTANT)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(qint64 pid READ pid NOTIFY runningChanged)
    Q_PROPERTY(qint64 gamescopePid READ gamescopePid NOTIFY gamescopePidChanged)
    Q_PROPERTY(QString username READ username NOTIFY configChanged)
    Q_PROPERTY(QRect windowGeometry READ windowGeometry NOTIFY configChanged)

public:
    explicit GamescopeInstance(QObject *parent = nullptr);
    ~GamescopeInstance() override;

    Q_INVOKABLE bool start(const QVariantMap &config, int index);

    Q_INVOKABLE void stop(int timeoutMs = 5000);
    Q_INVOKABLE void kill();

    bool isRunning() const;

    int index() const { return m_index; }
    qint64 pid() const { return m_helperPid; }
    qint64 gamescopePid() const { return m_gamescopePid; }
    QString status() const { return m_status; }
    QString username() const { return m_username; }
    QRect windowGeometry() const { return m_windowGeometry; }

    static QStringList buildGamescopeArgs(const QVariantMap &config);

    static QStringList buildEnvironment(const QVariantMap &config);

Q_SIGNALS:
    void runningChanged();
    void statusChanged();
    void configChanged();
    void gamescopePidChanged();
    void started();
    void stopped();
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void onHelperInstanceStopped(const QString &username, qint64 pid, const QString &reason);

private:
    void setStatus(const QString &status);
    static qint64 resolveGamescopePid(qint64 launchedPid);

    int m_index = -1;
    QString m_status;
    QString m_username;
    QRect m_windowGeometry;
    qint64 m_helperPid = 0;
    qint64 m_gamescopePid = 0;
};
