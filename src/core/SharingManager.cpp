// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SharingManager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <KSharedConfig>
#include <KConfigGroup>

#include <pwd.h>
#include <unistd.h>

// Blacklisted paths that should never be shared
static const QStringList BLACKLISTED_PATHS = {
    QStringLiteral("/"),
    QStringLiteral("/etc"),
    QStringLiteral("/var"),
    QStringLiteral("/usr"),
    QStringLiteral("/bin"),
    QStringLiteral("/sbin"),
    QStringLiteral("/lib"),
    QStringLiteral("/lib64"),
    QStringLiteral("/home"),
    QStringLiteral("/root"),
    QStringLiteral("/boot"),
    QStringLiteral("/proc"),
    QStringLiteral("/sys"),
    QStringLiteral("/dev"),
    QStringLiteral("/run"),
    QStringLiteral("/tmp")
};

SharingManager::SharingManager(QObject *parent)
    : QObject(parent)
{
    // Determine current user's home directory
    m_userHome = currentUserHome();
    
    // Load saved directories from config
    loadFromConfig();
}

void SharingManager::addDirectory(const QString &path, const QString &alias)
{
    // Normalize path (remove trailing slashes)
    QString normalizedPath = path;
    while (normalizedPath.endsWith(QLatin1Char('/')) && normalizedPath.length() > 1) {
        normalizedPath.chop(1);
    }

    // Validate the path
    if (!isValidPath(normalizedPath)) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid path: %1").arg(normalizedPath));
        return;
    }

    // Check if already added
    if (isDuplicate(normalizedPath)) {
        Q_EMIT errorOccurred(QStringLiteral("Directory already shared: %1").arg(normalizedPath));
        return;
    }

    // Check blacklist
    if (isBlacklisted(normalizedPath)) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot share system directory: %1").arg(normalizedPath));
        return;
    }

    // Create the entry
    SharedDirectory dir;
    dir.sourcePath = normalizedPath;
    
    // Only set alias for non-home paths
    if (isOutsideHome(normalizedPath)) {
        dir.mountAlias = alias;
    }
    // For home-relative paths, alias stays empty (computed automatically)

    m_directories.append(dir);
    saveToConfig();
    
    if (alias.isEmpty()) {
        qDebug() << "SharingManager: Added directory" << normalizedPath;
    } else {
        qDebug() << "SharingManager: Added directory" << normalizedPath << "->" << alias;
    }
    
    Q_EMIT sharedDirectoriesChanged();
}

void SharingManager::removeDirectory(const QString &path)
{
    // Normalize path
    QString normalizedPath = path;
    while (normalizedPath.endsWith(QLatin1Char('/')) && normalizedPath.length() > 1) {
        normalizedPath.chop(1);
    }

    for (int i = 0; i < m_directories.size(); ++i) {
        if (m_directories.at(i).sourcePath == normalizedPath) {
            m_directories.removeAt(i);
            saveToConfig();
            
            qDebug() << "SharingManager: Removed directory" << normalizedPath;
            
            Q_EMIT sharedDirectoriesChanged();
            return;
        }
    }

    qWarning() << "SharingManager: Directory not found:" << normalizedPath;
}

bool SharingManager::isOutsideHome(const QString &path) const
{
    return !path.startsWith(m_userHome);
}

QStringList SharingManager::directoryList() const
{
    QStringList result;
    for (const SharedDirectory &dir : m_directories) {
        // Format: "source|alias"
        result << dir.sourcePath + QLatin1Char('|') + dir.mountAlias;
    }
    return result;
}

QVariantList SharingManager::sharedDirectoriesAsVariant() const
{
    QVariantList result;
    for (const SharedDirectory &dir : m_directories) {
        QVariantMap map;
        map[QStringLiteral("sourcePath")] = dir.sourcePath;
        map[QStringLiteral("mountAlias")] = dir.mountAlias;
        result.append(map);
    }
    return result;
}

QString SharingManager::steamLibraryPath() const
{
    // Check for standard Steam library location
    QString steamApps = m_userHome + QStringLiteral("/.local/share/Steam/steamapps");
    if (QDir(steamApps).exists()) {
        return steamApps;
    }
    
    // Check for Flatpak Steam
    QString flatpakSteam = m_userHome + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam/steamapps");
    if (QDir(flatpakSteam).exists()) {
        return flatpakSteam;
    }

    return QString();
}

void SharingManager::loadFromConfig()
{
    m_directories.clear();

    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Sharing"));

    int count = group.readEntry(QStringLiteral("DirectoryCount"), 0);
    
    for (int i = 0; i < count; ++i) {
        QString key = QStringLiteral("Directory%1").arg(i);
        QString value = group.readEntry(key, QString());
        
        if (value.isEmpty()) {
            continue;
        }

        // Parse "source|alias" format
        QStringList parts = value.split(QLatin1Char('|'));
        if (parts.isEmpty()) {
            continue;
        }

        SharedDirectory dir;
        dir.sourcePath = parts.at(0);
        dir.mountAlias = parts.size() > 1 ? parts.at(1) : QString();

        // Validate the path still exists
        if (QDir(dir.sourcePath).exists()) {
            m_directories.append(dir);
        } else {
            qWarning() << "SharingManager: Configured directory no longer exists:" << dir.sourcePath;
        }
    }

    qDebug() << "SharingManager: Loaded" << m_directories.size() << "shared directories from config";
}

void SharingManager::saveToConfig()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Sharing"));

    // Clear old entries
    group.deleteGroup();
    group = config->group(QStringLiteral("Sharing"));

    group.writeEntry(QStringLiteral("DirectoryCount"), m_directories.size());

    for (int i = 0; i < m_directories.size(); ++i) {
        QString key = QStringLiteral("Directory%1").arg(i);
        QString value = m_directories.at(i).sourcePath + QLatin1Char('|') + m_directories.at(i).mountAlias;
        group.writeEntry(key, value);
    }

    config->sync();
    
    qDebug() << "SharingManager: Saved" << m_directories.size() << "shared directories to config";
}

bool SharingManager::isValidPath(const QString &path) const
{
    // Must be absolute path
    if (!path.startsWith(QLatin1Char('/'))) {
        return false;
    }

    // Check for path traversal
    if (path.contains(QStringLiteral(".."))) {
        return false;
    }

    // Must exist
    if (!QDir(path).exists()) {
        return false;
    }

    // Must be a directory
    QFileInfo info(path);
    if (!info.isDir()) {
        return false;
    }

    return true;
}

bool SharingManager::isBlacklisted(const QString &path) const
{
    // Check exact matches
    for (const QString &blacklisted : BLACKLISTED_PATHS) {
        if (path == blacklisted) {
            return true;
        }
    }

    // Check if it's another user's home directory
    // Allow paths inside the current user's home, but not other homes
    if (path.startsWith(QStringLiteral("/home/"))) {
        QString homePrefix = QStringLiteral("/home/");
        QString afterHome = path.mid(homePrefix.length());
        
        // Get the username portion
        int slashPos = afterHome.indexOf(QLatin1Char('/'));
        QString username = slashPos > 0 ? afterHome.left(slashPos) : afterHome;
        
        // Check if this is a different user's home
        QString otherUserHome = homePrefix + username;
        if (!m_userHome.startsWith(otherUserHome)) {
            // This is a path in another user's home directory
            return true;
        }
    }

    return false;
}

bool SharingManager::isDuplicate(const QString &path) const
{
    for (const SharedDirectory &dir : m_directories) {
        if (dir.sourcePath == path) {
            return true;
        }
    }
    return false;
}

QString SharingManager::currentUserHome() const
{
    // Get home directory from passwd
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        return QString::fromLocal8Bit(pw->pw_dir);
    }
    
    // Fallback to environment
    return QDir::homePath();
}
