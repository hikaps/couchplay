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
#include <QStringList>
#include <QUrl>

#include "couchplay-version.h"

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    m_installMethod = detectInstallMethod();
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
    const QStringList aParts = normalizeVersion(a).split(QLatin1Char('.'));
    const QStringList bParts = normalizeVersion(b).split(QLatin1Char('.'));

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
            return cmp;
        }
    }
    return 0;
}

QString UpdateChecker::parseLatestTag(const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return QString();
    }
    const QJsonValue tag = doc.object().value(QStringLiteral("tag_name"));
    if (!tag.isString()) {
        return QString();
    }
    return tag.toString();
}

QString UpdateChecker::detectInstallMethod()
{
    if (QFile::exists(QStringLiteral("/.flatpak-info"))) {
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

void UpdateChecker::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
}
