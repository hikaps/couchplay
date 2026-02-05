// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QStringList>
#include <QVariantList>
#include <qqmlintegration.h>

#include "HeroicConfigManager.h"
#include "SteamConfigManager.h"

/**
 * LauncherInfo - Launcher-specific configuration and paths
 * 
 * Associates a launcher with its config paths, game directories,
 * and capabilities (ACL requirements, shortcut sync support).
 */
struct LauncherInfo {
    Q_GADGET
    Q_PROPERTY(QString configPath MEMBER configPath)
    Q_PROPERTY(QString dataPath MEMBER dataPath)
    Q_PROPERTY(QStringList gameDirectories MEMBER gameDirectories)
    Q_PROPERTY(bool requiresAcls MEMBER requiresAcls)
    Q_PROPERTY(bool hasShortcutSync MEMBER hasShortcutSync)

public:
    QString configPath;           // Main config directory
    QString dataPath;             // Game data/library path
    QStringList gameDirectories;  // Paths needing ACLs (computed at runtime)
    bool requiresAcls = false;    // Does this launcher need ACL setup?
    bool hasShortcutSync = false; // Can we sync shortcuts from this launcher?
    
    bool operator==(const LauncherInfo &other) const {
        return configPath == other.configPath && 
               dataPath == other.dataPath &&
               requiresAcls == other.requiresAcls &&
               hasShortcutSync == other.hasShortcutSync;
    }
};

Q_DECLARE_METATYPE(LauncherInfo)

/**
 * @brief A launch preset defining how to start a game/application
 * 
 * Presets can be builtin (Steam, Heroic, Lutris) or custom (from .desktop files
 * or user-defined). Each preset specifies the command to run, optional
 * working directory, and whether Steam integration (-e flag) is enabled.
 * 
 * Launcher-aware presets include a launcherId and LauncherInfo with associated
 * config paths and directories that need ACL setup.
 */
struct LaunchPreset {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString command MEMBER command)
    Q_PROPERTY(QString workingDirectory MEMBER workingDirectory)
    Q_PROPERTY(QString iconName MEMBER iconName)
    Q_PROPERTY(QString desktopFilePath MEMBER desktopFilePath)
    Q_PROPERTY(bool isBuiltin MEMBER isBuiltin)
    Q_PROPERTY(bool steamIntegration MEMBER steamIntegration)

    Q_PROPERTY(QString launcherId MEMBER launcherId)
    Q_PROPERTY(LauncherInfo launcherInfo MEMBER launcherInfo)
    Q_PROPERTY(QStringList sharedDirectories MEMBER sharedDirectories)

public:
    QString id;                     // Unique ID (e.g., "steam", "heroic", "lutris", "custom-abc123")
    QString name;                   // Display name
    QString command;                // Launch command
    QString workingDirectory;       // Optional working dir (from .desktop Path=)
    QString iconName;               // Icon name for UI
    QString desktopFilePath;        // Source .desktop file (if applicable)
    bool isBuiltin = false;         // true for Steam/Heroic/Lutris
    bool steamIntegration = false;  // Enable gamescope -e flag
    
    // Launcher-aware fields
    QString launcherId;             // "steam", "heroic", "lutris", "custom" (empty for non-launcher presets)
    LauncherInfo launcherInfo;      // Populated by detection for launcher presets
    QStringList sharedDirectories;  // Per-preset shared directories for ACL/mount setup

    bool operator==(const LaunchPreset &other) const { return id == other.id; }
};

Q_DECLARE_METATYPE(LaunchPreset)

/**
 * @brief Manages launch presets for game/application launching
 * 
 * Provides builtin presets (Steam, Lutris), discovery of installed
 * applications via .desktop files, and custom preset management.
 * Custom presets are persisted to ~/.config/couchplay/presets.json.
 */
class PresetManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList presets READ presetsAsVariant NOTIFY presetsChanged)
    Q_PROPERTY(QVariantList availableApplications READ availableApplicationsAsVariant NOTIFY applicationsChanged)

public:
    explicit PresetManager(QObject *parent = nullptr);
    ~PresetManager() override;

    /**
     * @brief Set the HeroicConfigManager for Heroic integration
     */
    Q_INVOKABLE void setHeroicConfigManager(HeroicConfigManager *manager);
    
    /**
     * @brief Get the HeroicConfigManager
     */
    HeroicConfigManager *heroicConfigManager() const { return m_heroicConfigManager; }

    /**
     * @brief Set the SteamConfigManager for Steam library detection
     */
    Q_INVOKABLE void setSteamConfigManager(SteamConfigManager *manager);

    /**
     * @brief Get the SteamConfigManager
     */
    SteamConfigManager *steamConfigManager() const { return m_steamConfigManager; }

    /**
     * @brief Get all available presets (builtin + custom)
     */
    QList<LaunchPreset> presets() const;
    QVariantList presetsAsVariant() const;

    /**
     * @brief Get applications discovered from .desktop files
     * These are potential presets that can be added as custom presets
     */
    QList<LaunchPreset> availableApplications() const { return m_availableApplications; }
    QVariantList availableApplicationsAsVariant() const;

    /**
     * @brief Get a preset by ID
     * @param id Preset ID
     * @return The preset, or an empty preset if not found
     */
    Q_INVOKABLE LaunchPreset getPreset(const QString &id) const;

    /**
     * @brief Get the command for a preset
     * @param id Preset ID
     * @return The command string, or empty if not found
     */
    Q_INVOKABLE QString getCommand(const QString &id) const;

    /**
     * @brief Get the working directory for a preset
     * @param id Preset ID
     * @return The working directory, or empty if not specified
     */
    Q_INVOKABLE QString getWorkingDirectory(const QString &id) const;

    /**
     * @brief Check if a preset has Steam integration enabled
     * @param id Preset ID
     * @return true if Steam integration is enabled
     */
    Q_INVOKABLE bool getSteamIntegration(const QString &id) const;

    /**
     * @brief Get launcher ID for a preset
     * @param id Preset ID
     * @return Launcher ID ("steam", "heroic", "lutris", "custom", or empty)
     */
    Q_INVOKABLE QString getLauncherId(const QString &id) const;

    /**
     * @brief Get game directories for a launcher preset (for ACL setup)
     * @param id Preset ID
     * @return List of directories that need ACLs
     */
    Q_INVOKABLE QStringList getGameDirectories(const QString &id) const;

    /**
     * @brief Get shared directories for a preset
     * @param id Preset ID
     * @return List of absolute paths to shared directories
     */
    Q_INVOKABLE QStringList getSharedDirectories(const QString &id) const;

    /**
     * @brief Set shared directories for a preset
     * @param id Preset ID
     * @param directories List of absolute paths
     * @return true if updated successfully
     */
    Q_INVOKABLE bool setSharedDirectories(const QString &id, const QStringList &directories);

    /**
     * @brief Add a custom preset
     * @param name Display name
     * @param command Launch command
     * @param workingDirectory Optional working directory
     * @param iconName Optional icon name
     * @param steamIntegration Enable Steam integration (-e flag)
     * @return The ID of the created preset
     */
    Q_INVOKABLE QString addCustomPreset(const QString &name,
                                         const QString &command,
                                         const QString &workingDirectory = QString(),
                                         const QString &iconName = QString(),
                                         bool steamIntegration = false);

    /**
     * @brief Add a preset from a .desktop file
     * @param desktopFilePath Path to the .desktop file
     * @return The ID of the created preset, or empty on failure
     */
    Q_INVOKABLE QString addPresetFromDesktopFile(const QString &desktopFilePath);

    /**
     * @brief Update a custom preset
     * @param id Preset ID (must be a custom preset)
     * @param name New display name
     * @param command New launch command
     * @param workingDirectory New working directory
     * @param iconName New icon name
     * @param steamIntegration New Steam integration setting
     * @return true if updated successfully
     */
    Q_INVOKABLE bool updateCustomPreset(const QString &id,
                                         const QString &name,
                                         const QString &command,
                                         const QString &workingDirectory,
                                         const QString &iconName,
                                         bool steamIntegration);

    /**
     * @brief Remove a custom preset
     * @param id Preset ID (must be a custom preset)
     * @return true if removed successfully
     */
    Q_INVOKABLE bool removeCustomPreset(const QString &id);

    /**
     * @brief Scan for installed applications (.desktop files)
     * Results are available via availableApplications()
     */
    Q_INVOKABLE void scanApplications();

    /**
     * @brief Reload presets from disk
     */
    Q_INVOKABLE void refresh();

Q_SIGNALS:
    void presetsChanged();
    void applicationsChanged();
    void errorOccurred(const QString &message);

public:
    /**
     * @brief Clean Exec= field by removing freedesktop field codes
     * @param exec The raw Exec= value
     * @return Cleaned command string
     */
    static QString cleanExecCommand(const QString &exec);

private:
    void initBuiltinPresets();
    void loadCustomPresets();
    void saveCustomPresets();
    
    /**
     * @brief Parse a .desktop file and extract preset information
     * @param filePath Path to the .desktop file
     * @return The parsed preset (id will be empty on failure)
     */
    LaunchPreset parseDesktopFile(const QString &filePath) const;

    /**
     * @brief Generate a unique ID for a custom preset
     */
    static QString generateCustomId();

    QString configFilePath() const;

    /**
     * @brief Get default shared directories for a built-in preset
     * @param id Preset ID ("steam", "heroic", "lutris")
     * @return List of auto-detected directories for this preset
     */
    QStringList getDefaultSharedDirectories(const QString &id) const;

    HeroicConfigManager *m_heroicConfigManager = nullptr;
    SteamConfigManager *m_steamConfigManager = nullptr;
    QList<LaunchPreset> m_builtinPresets;
    QList<LaunchPreset> m_customPresets;
    QList<LaunchPreset> m_availableApplications;
};
