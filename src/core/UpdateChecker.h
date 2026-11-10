// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <qqmlintegration.h>

#include <functional>

class QNetworkAccessManager;
class QProcess;

/**
 * @brief Checks GitHub for a newer stable release and, on Flatpak, updates the app in place.
 *
 * The checker only informs: it never updates the root helper or the SteamOS system extension.
 * CouchPlay ships as a Flatpak bundle (no remote), so `flatpak update` cannot update it. On Flatpak
 * installs updateNow() instead downloads the release bundle, verifies its checksum and installs it
 * on the host through HostProcess.
 */
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
    Q_PROPERTY(UpdateNowState updateNowState READ updateNowState NOTIFY updateNowStateChanged)
    Q_PROPERTY(QString updateNowMessage READ updateNowMessage NOTIFY updateNowMessageChanged)
public:
    enum State { Idle, Checking, UpToDate, UpdateAvailable, Error };
    Q_ENUM(State)

    enum UpdateNowState { UpdateNowIdle, UpdateNowRunning, UpdateNowSucceeded, UpdateNowFailed };
    Q_ENUM(UpdateNowState)

    /** Configures the process that downloads and installs the bundle of release @p tag (program, arguments, environment). */
    using ProcessConfigurator = std::function<void(QProcess &, const QString &tag)>;

    explicit UpdateChecker(QObject *parent = nullptr);
    /** Injectable install method and update-process configuration (tests). */
    UpdateChecker(const QString &installMethod, ProcessConfigurator configurator, QObject *parent = nullptr);

    Q_INVOKABLE void checkForUpdates(); // manual: Settings page
    Q_INVOKABLE void checkForUpdatesOnStartup(); // automatic: Main.qml only, emits checkFinished(automatic=true)
    /** Downloads, verifies and installs the latest release bundle on the host. Only valid for Flatpak installs with a known update. */
    Q_INVOKABLE void updateNow();

    static QString normalizeVersion(const QString &version);
    /** Negative, zero or positive as @p a is older than, equal to or newer than @p b. Pre-releases sort before their release. */
    static int compareVersions(const QString &a, const QString &b);
    /** Tag of a stable release object, or empty when the JSON is invalid or the release is a draft/pre-release. */
    static QString parseLatestTag(const QByteArray &json);
    static QString detectInstallMethod();
    /** Whether @p tag looks like a release tag (`v1.2.3`); only such tags are ever handed to the install script. */
    static bool isValidReleaseTag(const QString &tag);
    /** Fixed POSIX shell script run on the host; the release tag is its only argument ($1) and is never part of the text. */
    static QString updateScript();

    void applyLatestTag(const QString &tag); // called by the reply handler; public for tests

    State state() const;
    QString currentVersion() const;
    QString latestVersion() const;
    bool updateAvailable() const;
    QString installMethod() const;
    QString releasesUrl() const;
    UpdateNowState updateNowState() const;
    QString updateNowMessage() const;

Q_SIGNALS:
    void stateChanged();
    void latestVersionChanged();
    void updateAvailableChanged();
    void updateNowStateChanged();
    void updateNowMessageChanged();
    void checkFinished(bool automatic, bool updateAvailable, const QString &latestVersion);

private:
    void startCheck(bool automatic);
    void setState(State state);
    void setUpdateNowState(UpdateNowState state);
    void setUpdateNowMessage(const QString &message);
    void finishUpdateNow(bool success);
    void captureUpdateOutput(QProcess *process);

    QNetworkAccessManager *m_nam = nullptr;
    State m_state = Idle;
    QString m_latestVersion;
    QString m_latestTag;
    bool m_updateAvailable = false;
    bool m_automatic = false;
    QString m_installMethod;
    ProcessConfigurator m_configurator;
    UpdateNowState m_updateNowState = UpdateNowIdle;
    QString m_updateNowMessage;
};
