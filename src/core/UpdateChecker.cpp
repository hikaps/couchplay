// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "UpdateChecker.h"

#include <KLocalizedString>

#include <utility>

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
#include "couchplay-buildinfo.h"

namespace
{
void configureHostBundleInstall(QProcess &process, const QString &tag)
{
    // The script text is fixed; the tag travels only as a positional argument ($1).
    HostProcess::configure(process,
                           QStringLiteral("/bin/sh"),
                           {QStringLiteral("-c"), UpdateChecker::updateScript(), QStringLiteral("sh"), tag});
}
}

UpdateChecker::UpdateChecker(QObject *parent)
    : UpdateChecker(detectInstallMethod(), configureHostBundleInstall, parent)
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
        // A manual request joins the running check, so its result is not reported as automatic-only
        m_automatic = m_automatic && automatic;
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
    m_latestTag = tag.trimmed();
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
    if (!m_updateAvailable || !isValidReleaseTag(m_latestTag)) {
        qWarning() << "UpdateChecker: updateNow() without a known, valid update tag:" << m_latestTag;
        setUpdateNowMessage(i18nc("@info", "No valid update is known. Check for updates first."));
        setUpdateNowState(UpdateNowFailed);
        return;
    }
    m_updateOutputBuffer.clear();

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_configurator(*process, m_latestTag);

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
        captureUpdateOutput(process, true);
        finishUpdateNow(exitStatus == QProcess::NormalExit && exitCode == 0);
        process->deleteLater();
    });

    setUpdateNowState(UpdateNowRunning);
    process->start();
}

void UpdateChecker::captureUpdateOutput(QProcess *process, bool flush)
{
    m_updateOutputBuffer += process->readAll();

    // A read can end in the middle of a line, so only complete lines are shown until the process has finished.
    QByteArray ready;
    if (flush) {
        ready = std::exchange(m_updateOutputBuffer, QByteArray());
    } else {
        const qsizetype end = qMax(m_updateOutputBuffer.lastIndexOf('\n'), m_updateOutputBuffer.lastIndexOf('\r'));
        if (end < 0) {
            return;
        }
        ready = m_updateOutputBuffer.left(end + 1);
        m_updateOutputBuffer.remove(0, end + 1);
    }

    // flatpak redraws progress with carriage returns; the last non-empty line is the current status
    const QStringList lines =
        QString::fromUtf8(ready).split(QRegularExpression(QStringLiteral("[\r\n]")), Qt::SkipEmptyParts);
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

bool UpdateChecker::isValidReleaseTag(const QString &tag)
{
    static const QRegularExpression pattern(QRegularExpression::anchoredPattern(QStringLiteral("v?[0-9]+(\\.[0-9]+)*")));
    return pattern.match(tag).hasMatch();
}

QString UpdateChecker::updateScript()
{
    // Runs on the host with the (already validated) release tag as $1. Each line printed becomes the progress
    // message, so the last line is always the stage or the reason for failure.
    return QStringLiteral(R"SH(
tag=$1
case $tag in
    ''|*[!v0-9.]*) echo "Refusing to update: invalid release tag"; exit 2 ;;
esac
app=io.github.hikaps.couchplay
base=https://github.com/hikaps/couchplay/releases/download/$tag
dir=$(mktemp -d) || { echo "Could not create a temporary directory"; exit 1; }
cleanup() { rm -rf -- "$dir"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
cd "$dir" || exit 1
echo "Downloading CouchPlay $tag..."
for f in couchplay.flatpak couchplay.flatpak.sha256; do
    curl -fsSL --retry 3 --retry-delay 2 --connect-timeout 15 --speed-limit 1024 --speed-time 60 -o "$f" "$base/$f" || { echo "Download failed: $f could not be fetched from GitHub"; exit 1; }
done
echo "Verifying download..."
sha256sum -c couchplay.flatpak.sha256 >/dev/null 2>&1 || { echo "Checksum verification failed: the download does not match its published checksum. Nothing was installed."; exit 1; }
inst=$(flatpak list --app --columns=application,installation 2>/dev/null | awk -F'\t' -v id="$app" '$1 == id { print $2; exit }')
case $inst in
    ''|user) flag=--user; inst=user ;;
    system) flag=--system ;;
    *[!A-Za-z0-9_.-]*) echo "Unsupported Flatpak installation name: $inst"; exit 1 ;;
    *) flag=--installation=$inst ;;
esac
echo "Installing CouchPlay $tag ($inst installation)..."
if ! flatpak install -y --noninteractive "$flag" --bundle couchplay.flatpak; then
    echo "Installing failed. Install manually: download couchplay.flatpak from the CouchPlay releases page and run: flatpak install $flag couchplay.flatpak (system installations need sudo)"
    exit 1
fi
echo "Update installed."
)SH");
}

UpdateChecker::State UpdateChecker::state() const
{
    return m_state;
}

QString UpdateChecker::currentVersion() const
{
    return QStringLiteral(COUCHPLAY_VERSION_STRING);
}

QString UpdateChecker::displayVersion() const
{
    return QStringLiteral(COUCHPLAY_DISPLAY_VERSION);
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
