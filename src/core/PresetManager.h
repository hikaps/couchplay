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

struct DataDirectory {
    Q_GADGET
    Q_PROPERTY(QString path MEMBER path)
    Q_PROPERTY(QString mode MEMBER mode) // "copy", "overlay", "acl"

public:
    QString path;
    QString mode = QStringLiteral("acl");

    bool operator==(const DataDirectory &other) const {
        return path == other.path && mode == other.mode;
    }
};

Q_DECLARE_METATYPE(DataDirectory)

struct LauncherInfo {
    Q_GADGET
    Q_PROPERTY(QString configPath MEMBER configPath)
    Q_PROPERTY(QString dataPath MEMBER dataPath)

public:
    QString configPath;
    QString dataPath;

    bool operator==(const LauncherInfo &other) const {
        return configPath == other.configPath &&
               dataPath == other.dataPath;
    }
};

Q_DECLARE_METATYPE(LauncherInfo)

struct LaunchPreset {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString command MEMBER command)
    Q_PROPERTY(QString workingDirectory MEMBER workingDirectory)
    Q_PROPERTY(QString iconName MEMBER iconName)
    Q_PROPERTY(QString desktopFilePath MEMBER desktopFilePath)
    Q_PROPERTY(bool isBuiltin MEMBER isBuiltin)

    Q_PROPERTY(QString launcherId MEMBER launcherId)
    Q_PROPERTY(LauncherInfo launcherInfo MEMBER launcherInfo)
    Q_PROPERTY(QList<DataDirectory> dataDirectories MEMBER dataDirectories)
    Q_PROPERTY(QString flatpakAppId MEMBER flatpakAppId)
    Q_PROPERTY(QString flatpakArgs MEMBER flatpakArgs)

public:
    QString id;                     // e.g., "steam", "heroic", "lutris", "custom-abc123"
    QString name;
    QString command;
    QString workingDirectory;       // Optional, from .desktop Path=
    QString iconName;
    QString desktopFilePath;        // Source .desktop file (if applicable)
    bool isBuiltin = false;         // true for Steam/Heroic/Lutris

    QString launcherId;             // "steam", "heroic", "lutris", "custom" (empty for non-launcher presets)
    LauncherInfo launcherInfo;      // Populated by detection for launcher presets
    QList<DataDirectory> dataDirectories; // Per-preset data directories with mode (copy/overlay/acl)
    QString flatpakAppId;           // e.g., "com.valvesoftware.Steam" (empty = no Flatpak alternative)
    QString flatpakArgs;            // Extra args for Flatpak launch (e.g., "-tenfoot -steamdeck")

    bool operator==(const LaunchPreset &other) const { return id == other.id; }
};

Q_DECLARE_METATYPE(LaunchPreset)

/**
 * @brief Manages launch presets for game/application launching
 *
 * Provides builtin presets (Steam, Lutris), discovery of installed
 * applications via .desktop files, and custom preset management.
 * Custom presets are persisted to ~/.config/couchplayrc as KConfig groups.
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

    Q_INVOKABLE void setHeroicConfigManager(HeroicConfigManager *manager);
    HeroicConfigManager *heroicConfigManager() const { return m_heroicConfigManager; }

    Q_INVOKABLE void setSteamConfigManager(SteamConfigManager *manager);
    SteamConfigManager *steamConfigManager() const { return m_steamConfigManager; }

    QList<LaunchPreset> presets() const;
    QVariantList presetsAsVariant() const;

    /**
     * @brief Applications discovered from .desktop files, available as potential custom presets
     */
    QList<LaunchPreset> availableApplications() const { return m_availableApplications; }
    QVariantList availableApplicationsAsVariant() const;

    Q_INVOKABLE LaunchPreset getPreset(const QString &id) const;
    Q_INVOKABLE QString getCommand(const QString &id) const;
    Q_INVOKABLE QString getWorkingDirectory(const QString &id) const;
    Q_INVOKABLE QString getLauncherId(const QString &id) const;

    /**
     * @brief Get data directories for a preset
     * @param id Preset ID
     * @return QVariantList of DataDirectory gadgets
     */
    Q_INVOKABLE QVariantList getDataDirectories(const QString &id) const;

    /**
     * @brief Set data directories for a preset and persist
     * @param id Preset ID
     * @param directories QVariantList of DataDirectory gadgets
     * @return true if set successfully
     */
    Q_INVOKABLE bool setDataDirectories(const QString &id, const QVariantList &directories);

    /**
     * @brief Add a custom preset
     * @param name Display name
     * @param command Launch command
     * @param workingDirectory Optional working directory
     * @param iconName Optional icon name
     * @return The ID of the created preset
     */
    Q_INVOKABLE QString addCustomPreset(const QString &name,
                                         const QString &command,
                                         const QString &workingDirectory = QString(),
                                         const QString &iconName = QString());

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
     * @return true if updated successfully
     */
    Q_INVOKABLE bool updateCustomPreset(const QString &id,
                                         const QString &name,
                                         const QString &command,
                                         const QString &workingDirectory,
                                         const QString &iconName);

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

    /**
     * @brief Detect launcher type from a command string
     * @param command The launch command (already cleaned of field codes)
     * @return Launcher ID ("steam", "heroic", "lutris") or empty string
     */
    QString detectLauncherId(const QString &command) const;

private:
    void initBuiltinPresets();
    void loadCustomPresets();
    void saveCustomPresets();
    void loadFlatpakCache();
    void saveFlatpakCache();
    QString resolveLaunchCommand(const QString &nativeCommand,
                                  const QString &flatpakAppId,
                                  const QString &flatpakArgs) const;

    /**
     * @brief Parse a .desktop file and extract preset information
     * @param filePath Path to the .desktop file
     * @return The parsed preset (id will be empty on failure)
     */
    LaunchPreset parseDesktopFile(const QString &filePath) const;

    static QString generateCustomId();

    void populateLauncherInfo(LaunchPreset &preset) const;

    /**
     * @brief Get default data directories for a built-in preset
     * @param id Preset ID ("steam", "heroic", "lutris")
     * @return List of auto-detected data directories with modes
     */
    QList<DataDirectory> getDefaultDataDirectories(const QString &id) const;

    HeroicConfigManager *m_heroicConfigManager = nullptr;
    SteamConfigManager *m_steamConfigManager = nullptr;
    QList<LaunchPreset> m_builtinPresets;
    QList<LaunchPreset> m_customPresets;
    QList<LaunchPreset> m_availableApplications;
};
