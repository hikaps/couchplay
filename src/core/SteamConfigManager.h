// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <qqmlintegration.h>

class CouchPlayHelperClient;

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
    QString steamRoot;           // ~/.steam/steam or ~/.local/share/Steam
    QString configDir;           // steamRoot/config
    QString userDataDir;         // steamRoot/userdata/<ID>
    QString libraryFoldersVdf;   // configDir/libraryfolders.vdf
    QString shortcutsVdf;        // userDataDir/config/shortcuts.vdf
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

public:
    explicit SteamConfigManager(QObject *parent = nullptr);
    ~SteamConfigManager() override = default;

    /**
     * Sync shortcuts enabled property
     */
    bool syncShortcutsEnabled() const { return m_syncShortcutsEnabled; }
    void setSyncShortcutsEnabled(bool enabled);

    /**
     * Set the helper client for privileged file operations
     */
    void setHelperClient(CouchPlayHelperClient *client);
    CouchPlayHelperClient *helperClient() const { return m_helperClient; }

    /**
     * Get detected Steam paths
     */
    SteamPaths steamPaths() const { return m_steamPaths; }

    /**
     * Check if Steam installation was detected
     */
    bool isSteamDetected() const { return m_steamPaths.valid; }

    /**
     * Get number of parsed shortcuts
     */
    int shortcutCount() const { return m_shortcuts.size(); }

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

Q_SIGNALS:
    void steamPathsChanged();
    void shortcutsLoaded();
    void helperClientChanged();
    void syncShortcutsEnabledChanged();
    void syncCompleted(const QString &username);
    void syncFailed(const QString &username, const QString &error);
    void errorOccurred(const QString &message);

private:
    // VDF parsing
    QList<SteamShortcut> parseShortcutsVdf(const QString &path);
    
    // Get target Steam paths for a user
    SteamPaths getTargetSteamPaths(const QString &username) const;
    
    CouchPlayHelperClient *m_helperClient = nullptr;
    SteamPaths m_steamPaths;
    QList<SteamShortcut> m_shortcuts;
    QString m_userHome;
    bool m_syncShortcutsEnabled = false;
};
