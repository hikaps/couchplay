// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <qqmlintegration.h>
#include <QString>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "SandboxCommand.h"
#include "SteamAccountDiscovery.h"
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

struct SteamGame {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QString installPath MEMBER installPath)
    Q_PROPERTY(QString source MEMBER source)

public:
    QString id;
    QString title;
    QString installPath;
    QString source; // native or shortcut
};

Q_DECLARE_METATYPE(SteamGame)

/**
 * SteamConfigManager - Manages Steam configuration sharing between the desktop user and players
 *
 * Writes shortcuts, library folders and compatibility tool mappings directly into each player's
 * home directory (PlayerStore::homeDir). Paths written into Steam's config files use the sandbox
 * view of the player's home, i.e. QDir::homePath(). Host Steam libraries are shared as overlay
 * mounts returned from prepareDataDir.
 */
class SteamConfigManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(SteamPaths steamPaths READ steamPaths NOTIFY steamPathsChanged)
    Q_PROPERTY(bool steamDetected READ isSteamDetected NOTIFY steamPathsChanged)
    Q_PROPERTY(int shortcutCount READ shortcutCount NOTIFY shortcutsLoaded)
    Q_PROPERTY(bool syncShortcutsEnabled READ syncShortcutsEnabled WRITE setSyncShortcutsEnabled NOTIFY syncShortcutsEnabledChanged)
    Q_PROPERTY(bool shareLibraryEnabled READ shareLibraryEnabled WRITE setShareLibraryEnabled NOTIFY shareLibraryEnabledChanged)
    Q_PROPERTY(int libraryCount READ libraryCount NOTIFY librariesLoaded)
    Q_PROPERTY(QVariantList libraries READ librariesAsVariant NOTIFY librariesLoaded)
    Q_PROPERTY(QVariantList sourceAccounts READ sourceAccounts NOTIFY sourceAccountsChanged)
    Q_PROPERTY(int sourceAccountIndex READ sourceAccountIndex NOTIFY sourceAccountsChanged)
    Q_PROPERTY(bool sourceAccountAvailable READ sourceAccountAvailable NOTIFY sourceAccountsChanged)
    Q_PROPERTY(QString sourceAccountError READ sourceAccountError NOTIFY sourceAccountsChanged)
    Q_PROPERTY(QVariantList games READ gamesAsVariant NOTIFY gamesLoaded)

public:
    explicit SteamConfigManager(QObject *parent = nullptr);
    ~SteamConfigManager() override = default;

    bool syncShortcutsEnabled() const
    {
        return m_syncShortcutsEnabled;
    }
    void setSyncShortcutsEnabled(bool enabled);

    SteamPaths steamPaths() const
    {
        return m_steamPaths;
    }

    bool isSteamDetected() const
    {
        return m_steamPaths.valid;
    }

    int shortcutCount() const
    {
        return m_shortcuts.size();
    }

    bool shareLibraryEnabled() const { return m_shareLibraryEnabled; }
    void setShareLibraryEnabled(bool enabled);

    int libraryCount() const { return m_libraries.size(); }
    QVariantList librariesAsVariant() const;
    QVariantList sourceAccounts() const;
    int sourceAccountIndex() const;
    bool sourceAccountAvailable() const;
    QString sourceAccountError() const;
    Q_INVOKABLE bool selectSourceAccount(const QString &root, const QString &accountId);
    Q_INVOKABLE void refreshSourceAccounts();
    bool isSteamRootPath(const QString &path) const;

    Q_INVOKABLE void detectSteamPaths();
    Q_INVOKABLE QString getSteamUserId() const;
    QString getTargetSteamUserId(const QString &player) const;
    Q_INVOKABLE void loadShortcuts();
    Q_INVOKABLE void loadGames();
    void loadLibraryFolders();

    Q_INVOKABLE QVariantList shortcutsAsVariant() const;
    Q_INVOKABLE QVariantList gamesAsVariant() const;
    Q_INVOKABLE QStringList extractShortcutDirectories() const;

    bool isSteamBootstrapped(const QString &player) const;
    SteamPaths getTargetSteamPaths(const QString &player) const;
    bool syncShortcutsToUser(const QString &player);
    bool cleanupLibrarySharing(const QString &player);
    bool prepareDataDir(const DataDirectory &dir, const QString &player, QList<SandboxMount> &mounts);
    bool finalizeDataDir(const DataDirectory &dir, const QString &player);

    /**
     * Names of the Steam tools (Proton, Steam Linux Runtime) in the host libraries whose manifest says an update is
     * pending. A player's Steam starts that update inside its own read-only view and breaks its copy of the tool.
     */
    QStringList pendingToolUpdates() const;

    /**
     * Removes the tool folders (Proton*, SteamLinuxRuntime*) under @p upperDir, a player's overlay layer for a
     * library's steamapps/common, that hold overlay whiteouts: deletions a failed tool update left behind, which hide
     * the host's files from the player. Returns the names of the folders removed.
     */
    static QStringList clearToolWhiteouts(const QString &upperDir);

Q_SIGNALS:
    void steamPathsChanged();
    void shortcutsLoaded();
    void gamesLoaded();
    void syncShortcutsEnabledChanged();
    void shareLibraryEnabledChanged();
    void librariesLoaded();
    void syncCompleted(const QString &username);
    void syncFailed(const QString &username, const QString &error);
    void sourceAccountsChanged();
    void errorOccurred(const QString &message);

private:
    QList<SteamShortcut> parseShortcutsVdf(const QString &path);
    QList<SteamLibraryFolder> parseLibraryFoldersVdf(const QString &path);
    QList<SteamGame> parseInstalledGames() const;
    QString generateLibraryFoldersVdf(const QList<SteamLibraryFolder> &libraries);
    QString findTargetSteamRoot(const QString &player) const;
    void mirrorCompatToolMapping(const QString &player, const QSet<QString> &sharedAppIds);
    void detectSteamPathsInternal(bool allowAutomaticSelection);

    SteamPaths m_steamPaths;
    QList<SteamShortcut> m_shortcuts;
    QList<SteamLibraryFolder> m_libraries;
    QList<SteamGame> m_games;
    QString m_userHome;
    bool m_syncShortcutsEnabled = false;
    QList<SteamAccount> m_sourceAccounts;
    QString m_sourceSteamRoot;
    QString m_sourceSteamAccountId;
    bool m_shareLibraryEnabled = false;
};