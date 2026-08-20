// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QObject>
#include <qqmlintegration.h>
#include <QByteArray>
#include <QString>

class QNetworkAccessManager;

class UpdateChecker : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY latestVersionChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateAvailableChanged)
    Q_PROPERTY(QString installMethod READ installMethod CONSTANT)
    Q_PROPERTY(QString releasesUrl READ releasesUrl CONSTANT)
public:
    enum State { Idle, Checking, UpToDate, UpdateAvailable, Error }; // plain enum: Q_ENUM/QML-safe
    Q_ENUM(State)

    explicit UpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void checkForUpdates();          // manual — no passive notification
    Q_INVOKABLE void checkForUpdatesOnStartup(); // automatic — Main.qml only

    // Pure helpers, public for unit tests:
    static QString normalizeVersion(const QString &version);
    static int compareVersions(const QString &a, const QString &b);
    static QString parseLatestTag(const QByteArray &json);
    static QString detectInstallMethod();

    void applyLatestTag(const QString &tag); // public for unit tests; called by reply handler

    State state() const;
    QString currentVersion() const;
    QString latestVersion() const;
    bool updateAvailable() const;
    QString installMethod() const;
    QString releasesUrl() const { return QStringLiteral("https://github.com/hikaps/couchplay/releases"); }

Q_SIGNALS:
    void stateChanged();
    void latestVersionChanged();
    void updateAvailableChanged();
    void checkFinished(bool automatic, bool updateAvailable, const QString &latestVersion);

private:
    void startCheck(bool automatic);
    void setState(State state);
    QNetworkAccessManager *m_nam = nullptr;
    State m_state = Idle;
    QString m_latestVersion;
    bool m_updateAvailable = false;
    bool m_automatic = false;
    QString m_installMethod;
};
