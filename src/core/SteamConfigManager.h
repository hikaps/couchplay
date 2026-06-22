// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <qqmlintegration.h>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class CouchPlayHelperClient;
struct DataDirectory;

/**
 * SteamPaths - Detected Steam installation paths
 */
struct SteamPaths {
    Q_GADGET
    Q_PROPERTY(QString steamRoot MEMBER steamRoot)
    Q_PROPERTY(QString configDir MEMBER configDir)
    Q_PROPERTY(QString userDataDir MEMBER userDataDir)
    Q_PROPERTY(QString libraryFoldersVdf MEMBER libraryFoldersVdf)
    Q_PROPERTY(QString shortcutsVdf MEMBER shortcutsVdf)
    Q_PROPERTY(bool valid MEMBER valid)

public:
    QString steamRoot; // ~/.steam/steam or ~/.local/share/Steam
    QString configDir; // steamRoot/config
    QString userDataDir; // steamRoot/userdata/<ID>
    QString libraryFoldersVdf; // configDir/libraryfolders.vdf
    QString shortcutsVdf; // userDataDir/config/shortcuts.vdf
    bool valid = false;
};

Q_DECLARE_METATYPE(SteamPaths)

/**
 * SteamShortcut - Represents a non-Steam game shortcut
 */
struct SteamShortcut {
    Q_GADGET
    Q_PROPERTY(quint32 appId MEMBER appId)
    Q_PROPERTY(QString appName MEMBER appName)
    Q_PROPERTY(QString exe MEMBER exe)
    Q_PROPERTY(QString startDir MEMBER startDir)
    Q_PROPERTY(QString icon MEMBER icon)
    Q_PROPERTY(QString launchOptions MEMBER launchOptions)
public:
    quint32 appId = 0;
    QString appName;
    QString exe;
    QString startDir;
    QString icon;
    QString shortcutPath;
    QString launchOptions;
    bool isHidden = false;
    bool allowDesktopConfig = true;
    bool allowOverlay = true;
    bool openVR = false;
    bool devkit = false;
    QString devkitGameId;
    quint32 devkitOverrideAppId = 0;
    quint32 lastPlayTime = 0;
    QString flatpakAppId;
    QString sortAs;
    QStringList tags;
};

Q_DECLARE_METATYPE(SteamShortcut)

/**
 * SteamLibraryFolder - Represents a Steam library folder
 */
struct SteamLibraryFolder {
    Q_GADGET
    Q_PROPERTY(QString path MEMBER path)
    Q_PROPERTY(QString label MEMBER label)
    Q_PROPERTY(quint64 totalSize MEMBER totalSize)
    Q_PROPERTY(QList<quint32> appIds MEMBER appIds)

public:
    QString path;
    QString label;
    quint64 totalSize = 0;
    QList<quint32> appIds;
};

Q_DECLARE_METATYPE(SteamLibraryFolder)

/**
 * SteamConfigManager - Manages Steam configuration sharing between users
 *
 * Handles syncing shortcuts from the compositor user to gaming users
 * during sessions. Sets ACLs on directories referenced in shortcuts
 * so gaming users can access them.
 */
class SteamConfigManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(SteamPaths steamPaths READ steamPaths NOTIFY steamPathsChanged)
    Q_PROPERTY(bool steamDetected READ isSteamDetected NOTIFY steamPathsChanged)
    Q_PROPERTY(int shortcutCount READ shortcutCount NOTIFY shortcutsLoaded)
    Q_PROPERTY(CouchPlayHelperClient* helperClient READ helperClient WRITE setHelperClient NOTIFY helperClientChanged)
    Q_PROPERTY(bool syncShortcutsEnabled READ syncShortcutsEnabled WRITE setSyncShortcutsEnabled NOTIFY syncShortcutsEnabledChanged)
    Q_PROPERTY(bool shareLibraryEnabled READ shareLibraryEnabled WRITE setShareLibraryEnabled NOTIFY shareLibraryEnabledChanged)
    Q_PROPERTY(int libraryCount READ libraryCount NOTIFY librariesLoaded)
    Q_PROPERTY(QVariantList libraries READ librariesAsVariant NOTIFY librariesLoaded)

public:
    explicit SteamConfigManager(QObject *parent = nullptr);
    ~SteamConfigManager() override = default;

    /**
     * Sync shortcuts enabled property
     */
    bool syncShortcutsEnabled() const
    {
        return m_syncShortcutsEnabled;
    }
    void setSyncShortcutsEnabled(bool enabled);

    /**
     * Set the helper client for privileged file operations
     */
    void setHelperClient(CouchPlayHelperClient *client);
    CouchPlayHelperClient *helperClient() const
    {
        return m_helperClient;
    }

    /**
     * Get detected Steam paths
     */
    SteamPaths steamPaths() const
    {
        return m_steamPaths;
    }

    /**
     * Check if Steam installation was detected
     */
    bool isSteamDetected() const
    {
        return m_steamPaths.valid;
    }

    /**
     * Get number of parsed shortcuts
     */
    int shortcutCount() const
    {
        return m_shortcuts.size();
    }

    bool shareLibraryEnabled() const { return m_shareLibraryEnabled; }
    void setShareLibraryEnabled(bool enabled);

    int libraryCount() const { return m_libraries.size(); }

    QVariantList librariesAsVariant() const;

    /**
     * Detect Steam installation paths
     */
    Q_INVOKABLE void detectSteamPaths();

    /**
     * Get the Steam user ID (from userdata directory) for compositor
     */
    Q_INVOKABLE QString getSteamUserId() const;

    /**
     * Get the Steam user ID for a target user
     * Looks in target user's ~/.steam/steam/userdata/ for their Steam ID
     *
     * @param username Target username
     * @return Steam user ID or empty string if not found
     */
    QString getTargetSteamUserId(const QString &username) const;

    /**
     * Load and parse shortcuts from the compositor's shortcuts.vdf
     */
    Q_INVOKABLE void loadShortcuts();

    void loadLibraryFolders();

    bool shareLibraryToUser(const QString &targetUsername);

    /**
     * Clean up library sharing state for a target user
     * Removes copied manifests and restores original libraryfolders.vdf
     */
    void cleanupLibrarySharing(const QString &targetUsername);

    /**
     * Get shortcuts as QVariantList for QML
     */
    Q_INVOKABLE QVariantList shortcutsAsVariant() const;

    /**
     * Extract unique directories from all shortcuts
     * Returns directories containing executables, start dirs, and icons
     * Used for setting ACLs on these directories
     *
     * @return List of unique directory paths
     */
    Q_INVOKABLE QStringList extractShortcutDirectories() const;

    /**
     * Sync shortcuts to a target user (simplified - no path rewriting)
     * Copies shortcuts.vdf to target user's Steam userdata folder
     * Uses ACLs for access instead of bind mounts
     *
     * @param targetUsername Username to sync to
     * @return true if successful
     */
    bool syncShortcutsToUser(const QString &targetUsername);

    /**
     * Prepare a data directory before the generic operation (copy/overlay/acl).
     * Dispatches to Steam-specific pre-processing based on dir.mode and dir.path.
     * Non-matching directories return true (no-op).
     *
     * @param dir Data directory to prepare
     * @param username Target username
     * @return true if successful or no-op
     */
    bool prepareDataDir(const DataDirectory &dir, const QString &username);

    /**
     * Finalize a data directory after the generic operation (copy/overlay/acl).
     * Dispatches to Steam-specific post-processing based on dir.mode and dir.path.
     * Non-matching directories return true (no-op).
     *
     * @param dir Data directory to finalize
     * @param username Target username
     * @return true if successful or no-op
     */
    bool finalizeDataDir(const DataDirectory &dir, const QString &username);

Q_SIGNALS:
    void steamPathsChanged();
    void shortcutsLoaded();
    void helperClientChanged();
    void syncShortcutsEnabledChanged();
    void shareLibraryEnabledChanged();
    void librariesLoaded();
    void syncCompleted(const QString &username);
    void syncFailed(const QString &username, const QString &error);
    void errorOccurred(const QString &message);

private:
    QList<SteamShortcut> parseShortcutsVdf(const QString &path);

    QList<SteamLibraryFolder> parseLibraryFoldersVdf(const QString &path);

    QString generateLibraryFoldersVdf(const QList<SteamLibraryFolder> &libraries);

    SteamPaths getTargetSteamPaths(const QString &username) const;

    CouchPlayHelperClient *m_helperClient = nullptr;
    SteamPaths m_steamPaths;
    QList<SteamShortcut> m_shortcuts;
    QList<SteamLibraryFolder> m_libraries;
    QString m_userHome;
    bool m_syncShortcutsEnabled = false;
    bool m_shareLibraryEnabled = false;
};
