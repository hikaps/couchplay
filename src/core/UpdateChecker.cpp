// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "UpdateChecker.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include "HostProcess.h"
#include "couchplay-version.h"

namespace
{
const QString kFlatpakAppId = QStringLiteral("io.github.hikaps.couchplay");

void configureHostFlatpakUpdate(QProcess &process)
{
    HostProcess::configure(process, QStringLiteral("flatpak"), {QStringLiteral("update"), QStringLiteral("-y"), kFlatpakAppId});
}
}

UpdateChecker::UpdateChecker(QObject *parent)
    : UpdateChecker(detectInstallMethod(), configureHostFlatpakUpdate, parent)
{
}

UpdateChecker::UpdateChecker(const QString &installMethod, ProcessConfigurator configurator, QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_installMethod(installMethod)
    , m_configurator(std::move(configurator))
{
}

void UpdateChecker::checkForUpdates()
{
    startCheck(false);
}

void UpdateChecker::checkForUpdatesOnStartup()
{
    startCheck(true);
}

void UpdateChecker::startCheck(bool automatic)
{
    if (m_state == Checking) {
        return;
    }

    m_automatic = automatic;
    setState(Checking);

    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/hikaps/couchplay/releases/latest")));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("couchplay/%1").arg(currentVersion()));
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/vnd.github+json"));
    request.setTransferTimeout(10000);

    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "UpdateChecker: request failed:" << reply->errorString();
            setState(Error);
            return;
        }

        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) {
            qWarning() << "UpdateChecker: unexpected HTTP status:" << statusCode;
            setState(Error);
            return;
        }

        const QString tag = parseLatestTag(reply->readAll());
        if (tag.isEmpty()) {
            qWarning() << "UpdateChecker: could not parse latest release response";
            setState(Error);
            return;
        }

        applyLatestTag(tag);
        Q_EMIT checkFinished(m_automatic, m_updateAvailable, m_latestVersion);
    });
}

void UpdateChecker::applyLatestTag(const QString &tag)
{
    const QString normalized = normalizeVersion(tag);
    if (m_latestVersion != normalized) {
        m_latestVersion = normalized;
        Q_EMIT latestVersionChanged();
    }

    const bool available = compareVersions(m_latestVersion, currentVersion()) > 0;
    if (m_updateAvailable != available) {
        m_updateAvailable = available;
        Q_EMIT updateAvailableChanged();
    }

    setState(available ? UpdateAvailable : UpToDate);
    qDebug() << "UpdateChecker:" << (available ? "update available:" : "up to date") << m_latestVersion;
}

void UpdateChecker::updateNow()
{
    if (m_updateNowState == UpdateNowRunning) {
        return;
    }

    setUpdateNowMessage(QString());
    if (m_installMethod != QLatin1String("flatpak")) {
        qWarning() << "UpdateChecker: updateNow() is only supported for Flatpak installs";
        setUpdateNowState(UpdateNowFailed);
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_configurator(*process);

    connect(process, &QProcess::readyRead, this, [this, process]() {
        captureUpdateOutput(process);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) {
            return; // Crashed is reported through finished()
        }
        setUpdateNowMessage(process->errorString());
        finishUpdateNow(false);
        process->deleteLater();
    });
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
        captureUpdateOutput(process);
        finishUpdateNow(exitStatus == QProcess::NormalExit && exitCode == 0);
        process->deleteLater();
    });

    setUpdateNowState(UpdateNowRunning);
    process->start();
}

void UpdateChecker::captureUpdateOutput(QProcess *process)
{
    // flatpak redraws progress with carriage returns; the last non-empty line is the current status
    const QStringList lines = QString::fromUtf8(process->readAll()).split(QRegularExpression(QStringLiteral("[\r\n]")), Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (!line.isEmpty()) {
            setUpdateNowMessage(line);
            return;
        }
    }
}

void UpdateChecker::finishUpdateNow(bool success)
{
    if (m_updateNowState != UpdateNowRunning) {
        return;
    }
    setUpdateNowState(success ? UpdateNowSucceeded : UpdateNowFailed);
}

QString UpdateChecker::normalizeVersion(const QString &version)
{
    QString normalized = version.trimmed();
    if (normalized.startsWith(QLatin1Char('v')) || normalized.startsWith(QLatin1Char('V'))) {
        normalized.remove(0, 1);
    }
    return normalized;
}

int UpdateChecker::compareVersions(const QString &a, const QString &b)
{
    // "1.2.3-beta.1": dotted numeric core, then an optional pre-release suffix
    const auto split = [](const QString &version) {
        const QString normalized = normalizeVersion(version);
        const qsizetype dash = normalized.indexOf(QLatin1Char('-'));
        return dash < 0 ? qMakePair(normalized, QString()) : qMakePair(normalized.left(dash), normalized.mid(dash + 1));
    };
    const auto [aCore, aPre] = split(a);
    const auto [bCore, bPre] = split(b);

    const QStringList aParts = aCore.split(QLatin1Char('.'));
    const QStringList bParts = bCore.split(QLatin1Char('.'));

    const int maxParts = qMax(aParts.size(), bParts.size());
    for (int i = 0; i < maxParts; ++i) {
        const QString aPart = i < aParts.size() ? aParts.at(i) : QStringLiteral("0");
        const QString bPart = i < bParts.size() ? bParts.at(i) : QStringLiteral("0");

        bool aOk = false;
        bool bOk = false;
        const qlonglong aNum = aPart.toLongLong(&aOk);
        const qlonglong bNum = bPart.toLongLong(&bOk);

        int cmp = 0;
        if (aOk && bOk) {
            cmp = (aNum < bNum) ? -1 : (aNum > bNum) ? 1 : 0;
        } else {
            cmp = QString::compare(aPart, bPart, Qt::CaseInsensitive);
        }
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }

    if (aPre.isEmpty() != bPre.isEmpty()) {
        return aPre.isEmpty() ? 1 : -1; // a release is newer than its own pre-release
    }
    const int preCmp = QString::compare(aPre, bPre, Qt::CaseInsensitive);
    return preCmp < 0 ? -1 : preCmp > 0 ? 1 : 0;
}

QString UpdateChecker::parseLatestTag(const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return QString();
    }
    const QJsonObject release = doc.object();
    if (release.value(QStringLiteral("draft")).toBool() || release.value(QStringLiteral("prerelease")).toBool()) {
        return QString();
    }
    const QJsonValue tag = release.value(QStringLiteral("tag_name"));
    return tag.isString() ? tag.toString() : QString();
}

QString UpdateChecker::detectInstallMethod()
{
    if (HostProcess::runningInFlatpak() || QFile::exists(QStringLiteral("/.flatpak-info"))) {
        return QStringLiteral("flatpak");
    }

    QFile osRelease(QStringLiteral("/etc/os-release"));
    if (osRelease.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!osRelease.atEnd()) {
            const QString line = QString::fromUtf8(osRelease.readLine()).trimmed();
            if (!line.startsWith(QStringLiteral("ID="))) {
                continue;
            }
            QString value = line.mid(3);
            if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
                value = value.mid(1, value.size() - 2);
            }
            if (value == QLatin1String("steamos")) {
                return QStringLiteral("steamos");
            }
        }
    }

    return QStringLiteral("native");
}

UpdateChecker::State UpdateChecker::state() const
{
    return m_state;
}

QString UpdateChecker::currentVersion() const
{
    return QStringLiteral(COUCHPLAY_VERSION_STRING);
}

QString UpdateChecker::latestVersion() const
{
    return m_latestVersion;
}

bool UpdateChecker::updateAvailable() const
{
    return m_updateAvailable;
}

QString UpdateChecker::installMethod() const
{
    return m_installMethod;
}

QString UpdateChecker::releasesUrl() const
{
    return QStringLiteral("https://github.com/hikaps/couchplay/releases");
}

UpdateChecker::UpdateNowState UpdateChecker::updateNowState() const
{
    return m_updateNowState;
}

QString UpdateChecker::updateNowMessage() const
{
    return m_updateNowMessage;
}

void UpdateChecker::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
}

void UpdateChecker::setUpdateNowState(UpdateNowState state)
{
    if (m_updateNowState == state) {
        return;
    }
    m_updateNowState = state;
    Q_EMIT updateNowStateChanged();
}

void UpdateChecker::setUpdateNowMessage(const QString &message)
{
    if (m_updateNowMessage == message) {
        return;
    }
    m_updateNowMessage = message;
    Q_EMIT updateNowMessageChanged();
}
