// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SteamShortcutManager.h"

#include "SessionManager.h"
#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QVariantMap>

#include <algorithm>
#include <unistd.h>

namespace {

constexpr char CouchPlayFlatpakId[] = "io.github.hikaps.couchplay";
constexpr char SteamFlatpakId[] = "com.valvesoftware.Steam";

QString markerFor(const QString &profilePath)
{
    const QByteArray hash = QCryptographicHash::hash(profilePath.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("couchplay://profile/") + QString::fromLatin1(hash);
}

QString quoteVdf(const QString &value)
{
    QString escaped;
    escaped.reserve(value.size() + 2);
    escaped += QLatin1Char('"');
    for (const QChar character : value) {
        if (character == QLatin1Char('\\') || character == QLatin1Char('"')
                    || character == QLatin1Char('$') || character.unicode() == 0x60) {
        escaped += character;
    }
    escaped += QLatin1Char('"');
    return escaped;
}




bool isOwnedByCurrentUser(const QFileInfo &info)
{
    return info.ownerId() == static_cast<uint>(::geteuid());
}

bool hasSymlinkComponent(const QString &path)
{
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    QString current = QDir::rootPath();
    const QStringList components = absolutePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        if (current.endsWith(QLatin1Char('/'))) {
            current += component;
        } else {
            current += QLatin1Char('/') + component;
        }
        const QFileInfo info(current);
        if (info.exists() && info.isSymLink()) {
            return true;
        }
    }
    return false;
}

QStringList steamRoots()
{
    const QString home = QDir::homePath();
    return {
        home + QStringLiteral("/.steam/steam"),
        home + QStringLiteral("/.local/share/Steam"),
        home + QStringLiteral("/.var/app/") + QString::fromLatin1(SteamFlatpakId)
            + QStringLiteral("/.steam/steam"),
        home + QStringLiteral("/.var/app/") + QString::fromLatin1(SteamFlatpakId)
            + QStringLiteral("/.local/share/Steam"),
    };
}

bool isSteamFlatpakRoot(const QString &path)
{
    return path.contains(QStringLiteral("/.var/app/") + QString::fromLatin1(SteamFlatpakId) + QLatin1Char('/'));
}

} // namespace

SteamShortcutManager::SteamShortcutManager(QObject *parent)
    : QObject(parent)
{
}

QVariantList SteamShortcutManager::accounts() const
{
    QVariantList result;
    result.reserve(m_accounts.size());
    for (const Account &account : m_accounts) {
        result.append(QVariantMap{{QStringLiteral("root"), account.root},
                                  {QStringLiteral("accountId"), account.accountId},
                                  {QStringLiteral("kind"), account.kind},
                                  {QStringLiteral("label"), account.label},
                                  {QStringLiteral("shortcutsPath"), account.shortcutsPath}});
    }
    return result;
}

void SteamShortcutManager::setSessionManager(SessionManager *manager)
{
    if (m_sessionManager == manager) {
        return;
    }
    m_sessionManager = manager;
    Q_EMIT sessionManagerChanged();
}

void SteamShortcutManager::fail(const QString &message)
{
    Q_EMIT errorOccurred(message);
}

bool SteamShortcutManager::discoverAccounts()
{
    QSet<QString> rootsSeen;
    QRegularExpression accountIdExpression(QStringLiteral("\\A[0-9]+\\z"));

    for (const QString &candidate : steamRoots()) {
        const QFileInfo rootInfo(candidate);
        if (!rootInfo.exists() || !rootInfo.isDir()) {
            continue;
        }
        const QString root = rootInfo.canonicalFilePath();
        if (root.isEmpty() || rootsSeen.contains(root)) {
            continue;
        }
        rootsSeen.insert(root);

        const QString userdataPath = root + QStringLiteral("/userdata");
        const QFileInfo userdataInfo(userdataPath);
        if (!userdataInfo.exists() || !userdataInfo.isDir() || userdataInfo.isSymLink()
            || !isOwnedByCurrentUser(userdataInfo) || hasSymlinkComponent(userdataPath)) {
            continue;
        }

        const bool flatpak = isSteamFlatpakRoot(candidate);
        const QDir userdata(userdataPath);
        const QFileInfoList entries = userdata.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (entry.isSymLink() || !isOwnedByCurrentUser(entry) || !accountIdExpression.match(entry.fileName()).hasMatch()) {
                continue;
            }
            const QString accountRoot = entry.canonicalFilePath();
            if (accountRoot.isEmpty() || hasSymlinkComponent(accountRoot)) {
                continue;
            }

            Account account;
            account.root = root;
            account.accountId = entry.fileName();
            account.kind = flatpak ? QStringLiteral("flatpak") : QStringLiteral("native");
            account.label = QStringLiteral("%1 Steam · %2")
                                .arg(flatpak ? QStringLiteral("Flatpak") : QStringLiteral("Native"), account.accountId);
            account.shortcutsPath = accountRoot + QStringLiteral("/config/shortcuts.vdf");

            const QFileInfo configInfo(QFileInfo(account.shortcutsPath).absolutePath());
            if (configInfo.exists() && (configInfo.isSymLink() || !configInfo.isDir() || !isOwnedByCurrentUser(configInfo)
                                        || hasSymlinkComponent(configInfo.filePath()))) {
                continue;
            }
            m_accounts.append(account);
        }
    }

    std::sort(m_accounts.begin(), m_accounts.end(), [](const Account &left, const Account &right) {
        return left.root == right.root ? left.accountId < right.accountId : left.root < right.root;
    });
    return !m_accounts.isEmpty();
}

bool SteamShortcutManager::prepare(const QString &profileName)
{
    m_prepared = false;
    m_accounts.clear();
    Q_EMIT accountsChanged();

    if (!m_sessionManager || !SessionManager::isValidProfileName(profileName)) {
        fail(QStringLiteral("Invalid saved profile"));
        return false;
    }

    const QList<SessionProfile> profiles = m_sessionManager->savedProfiles();
    const auto profile = std::find_if(profiles.cbegin(), profiles.cend(), [&](const SessionProfile &candidate) {
        return candidate.name == profileName;
    });
    if (profile == profiles.cend() || profile->filePath.isEmpty()) {
        fail(QStringLiteral("Saved profile not found"));
        return false;
    }

    m_profileName = profile->name;
    m_profilePath = profile->filePath;
    if (!discoverAccounts()) {
        fail(QStringLiteral("No Steam accounts were found"));
        return false;
    }

    m_prepared = true;
    Q_EMIT accountsChanged();
    Q_EMIT prepared();
    return true;
}

bool SteamShortcutManager::validateWritePath(const QString &path, bool allowMissingFile, QString *error) const
{
    const QFileInfo fileInfo(path);
    if (hasSymlinkComponent(path)) {
        if (error) {
            *error = QStringLiteral("Steam shortcut path contains a symlink");
        }
        return false;
    }

    const QFileInfo parentInfo(fileInfo.absolutePath());
    if (!parentInfo.exists() || !parentInfo.isDir() || parentInfo.isSymLink() || !isOwnedByCurrentUser(parentInfo)
        || !parentInfo.isWritable()) {
        if (error) {
            *error = QStringLiteral("Steam shortcut directory is not a safe writable directory");
        }
        return false;
    }

    if (!fileInfo.exists()) {
        if (!allowMissingFile) {
            if (error) {
                *error = QStringLiteral("Steam shortcuts.vdf disappeared");
            }
            return false;
        }
        return true;
    }
    if (fileInfo.isSymLink() || !fileInfo.isFile() || !isOwnedByCurrentUser(fileInfo) || !fileInfo.isWritable()) {
        if (error) {
            *error = QStringLiteral("Steam shortcuts.vdf is not a safe writable file");
        }
        return false;
    }
    return true;
}

bool SteamShortcutManager::readShortcuts(const QString &path, QByteArray *bytes, bool *exists, QString *error) const
{
    if (!bytes || !exists) {
        if (error) {
            *error = QStringLiteral("Shortcut output is unavailable");
        }
        return false;
    }

    const QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        *exists = false;
        *bytes = SteamShortcutsVdf::emptyDocument();
        return true;
    }
    if (!validateWritePath(path, false, error)) {
        return false;
    }
    if (fileInfo.size() > SteamShortcutsVdf::MaxDocumentSize) {
        if (error) {
            *error = QStringLiteral("shortcuts.vdf is too large");
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QByteArray contents = file.read(SteamShortcutsVdf::MaxDocumentSize + 1);
    if (contents.size() > SteamShortcutsVdf::MaxDocumentSize || contents.size() != fileInfo.size()) {
        if (error) {
            *error = QStringLiteral("shortcuts.vdf could not be read safely");
        }
        return false;
    }
    *exists = true;
    *bytes = contents;
    return true;
}

bool SteamShortcutManager::writeAtomically(const QString &path,
                                           const QByteArray &bytes,
                                           QFileDevice::Permissions permissions,
                                           QString *error) const
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    if (permissions != QFileDevice::Permissions() && !file.setPermissions(permissions)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

QString SteamShortcutManager::nativeGameModePath() const
{
    const QStringList candidates = {QStringLiteral("/usr/bin/couchplay-gamemode"),
                                    QStringLiteral("/usr/local/bin/couchplay-gamemode")};
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.isExecutable() && !info.isSymLink()) {
            return candidate;
        }
    }
    return {};
}

QString SteamShortcutManager::shortcutExecutable(const Account &account, QString *launchOptions) const
{
    if (!launchOptions) {
        return {};
    }
    const QString profileOptions = QStringLiteral("--profile ") + quoteVdf(m_profileName)
        + QStringLiteral(" --start --exit-after-session");
    const bool couchPlayFlatpak = qEnvironmentVariable("FLATPAK_ID") == QString::fromLatin1(CouchPlayFlatpakId);
    const bool steamFlatpak = account.kind == QStringLiteral("flatpak");

    if (couchPlayFlatpak) {
        if (steamFlatpak) {
            *launchOptions = QStringLiteral("--host ") + quoteVdf(QStringLiteral("/usr/bin/flatpak"))
                + QStringLiteral(" run --command=couchplay-gamemode ") + QString::fromLatin1(CouchPlayFlatpakId)
                + QLatin1Char(' ') + profileOptions;
            return quoteVdf(QStringLiteral("/usr/bin/flatpak-spawn"));
        }
        *launchOptions = QStringLiteral("run --command=couchplay-gamemode ")
            + QString::fromLatin1(CouchPlayFlatpakId) + QLatin1Char(' ') + profileOptions;
        return quoteVdf(QStringLiteral("/usr/bin/flatpak"));
    }

    const QString gameMode = nativeGameModePath();
    if (gameMode.isEmpty()) {
        return {};
    }
    if (steamFlatpak) {
        *launchOptions = QStringLiteral("--host ") + quoteVdf(gameMode) + QLatin1Char(' ') + profileOptions;
        return quoteVdf(QStringLiteral("/usr/bin/flatpak-spawn"));
    }
    *launchOptions = profileOptions;
    return quoteVdf(gameMode);
}

void SteamShortcutManager::addToSteam(int accountIndex)
{
    if (!m_prepared || accountIndex < 0 || accountIndex >= m_accounts.size()) {
        fail(QStringLiteral("Select a Steam account first"));
        return;
    }
    if (!m_sessionManager) {
        fail(QStringLiteral("Session manager is unavailable"));
        return;
    }

    const Account &account = m_accounts.at(accountIndex);
    const QList<SessionProfile> profiles = m_sessionManager->savedProfiles();
    const auto profile = std::find_if(profiles.cbegin(), profiles.cend(), [&](const SessionProfile &candidate) {
        return candidate.name == m_profileName && candidate.filePath == m_profilePath;
    });
    if (profile == profiles.cend()) {
        fail(QStringLiteral("Saved profile changed or was deleted"));
        return;
    }

    QString error;
    if (!validateWritePath(account.shortcutsPath, true, &error)) {
        fail(error);
        return;
    }

    QByteArray oldShortcuts;
    bool existed = false;
    if (!readShortcuts(account.shortcutsPath, &oldShortcuts, &existed, &error)) {
        fail(error);
        return;
    }

    QString launchOptions;
    const QString executable = shortcutExecutable(account, &launchOptions);
    if (executable.isEmpty()) {
        fail(QStringLiteral("The installed couchplay-gamemode launcher was not found"));
        return;
    }

    SteamShortcut shortcut;
    shortcut.appName = QStringLiteral("CouchPlay - ") + m_profileName;
    shortcut.exe = executable;
    shortcut.startDir = quoteVdf(QStringLiteral("/"));
    shortcut.shortcutPath = markerFor(m_profilePath);
    shortcut.launchOptions = launchOptions;
    shortcut.tags = {QStringLiteral("CouchPlay")};

    QByteArray newShortcuts;
    if (!SteamShortcutsVdf::upsert(oldShortcuts, shortcut, &newShortcuts, &error)) {
        fail(QStringLiteral("Steam shortcuts are invalid: %1").arg(error));
        return;
    }
    const bool updated = newShortcuts != oldShortcuts;
    if (!updated) {
        Q_EMIT registrationFinished(m_profileName, false);
        return;
    }

    QByteArray currentShortcuts;
    bool currentExists = false;
    if (!readShortcuts(account.shortcutsPath, &currentShortcuts, &currentExists, &error)
        || currentExists != existed || currentShortcuts != oldShortcuts) {
        fail(error.isEmpty() ? QStringLiteral("Steam shortcuts changed while registering") : error);
        return;
    }

    const QFileDevice::Permissions permissions = existed ? QFileInfo(account.shortcutsPath).permissions()
                                                         : QFileDevice::Permissions();
    if (existed) {
        const QString backupPath = account.shortcutsPath + QStringLiteral(".backup");
        if (!validateWritePath(backupPath, true, &error)
            || !writeAtomically(backupPath, oldShortcuts, permissions, &error)) {
            fail(QStringLiteral("Steam shortcuts backup failed: %1").arg(error));
            return;
        }
    }
    if (!writeAtomically(account.shortcutsPath, newShortcuts, permissions, &error)) {
        fail(QStringLiteral("Steam shortcuts could not be saved: %1").arg(error));
        return;
    }

    Q_EMIT registrationFinished(m_profileName, true);
}
