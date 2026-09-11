// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QList>
#include <QObject>
#include <qqmlintegration.h>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <KConfig>
#include <KConfigGroup>

#include "PresetManager.h"

class CouchPlayHelperClient;

class SessionManager;

/**
 * @brief Configuration for a single gamescope instance
 */
struct InstanceConfig {
    Q_GADGET
    Q_PROPERTY(QString username MEMBER username)
    Q_PROPERTY(int monitor MEMBER monitor)
    Q_PROPERTY(int internalWidth MEMBER internalWidth)
    Q_PROPERTY(int internalHeight MEMBER internalHeight)
    Q_PROPERTY(int outputWidth MEMBER outputWidth)
    Q_PROPERTY(int outputHeight MEMBER outputHeight)
    Q_PROPERTY(int refreshRate MEMBER refreshRate)
    Q_PROPERTY(QString scalingMode MEMBER scalingMode)
    Q_PROPERTY(QString filterMode MEMBER filterMode)
    Q_PROPERTY(QList<int> devices MEMBER devices)
    Q_PROPERTY(QStringList deviceStableIds MEMBER deviceStableIds)
    Q_PROPERTY(QStringList deviceStableIdNames MEMBER deviceStableIdNames)
    Q_PROPERTY(QString gameCommand MEMBER gameCommand)
    Q_PROPERTY(QString steamAppId MEMBER steamAppId)
    Q_PROPERTY(QString presetId MEMBER presetId)
    Q_PROPERTY(QVariantList dataDirectories READ dataDirectoriesAsVariant WRITE setDataDirectoriesFromVariant)
    Q_PROPERTY(QString overrideGamePath MEMBER overrideGamePath)
    Q_PROPERTY(QStringList overrideFiles MEMBER overrideFiles)
    Q_PROPERTY(QStringList overridePatterns MEMBER overridePatterns)
    Q_PROPERTY(QString outputMode MEMBER outputMode)
    Q_PROPERTY(QString streamResolution MEMBER streamResolution)
    Q_PROPERTY(int streamFps MEMBER streamFps)
    Q_PROPERTY(int streamBitrate MEMBER streamBitrate)
    Q_PROPERTY(QString streamCodec MEMBER streamCodec)
    Q_PROPERTY(int sunshinePort MEMBER sunshinePort)

public:
    QString username;
    int monitor = 0;
    int internalWidth = 1920;
    int internalHeight = 1080;
    int outputWidth = 960;
    int outputHeight = 1080;
    int refreshRate = 60;
    QString scalingMode = QStringLiteral("fit");
    QString filterMode = QStringLiteral("linear");
    QList<int> devices; // Runtime: current event numbers
    QStringList deviceStableIds; // Persistent: stable IDs for profile save/load
    QStringList deviceStableIdNames; // Persistent: friendly names (parallel to stableIds)
    QString gameCommand;
    QString steamAppId; // Steam App ID for Steam launch mode
    QString presetId = QStringLiteral("steam"); // ID of the launch preset to use
    QList<DataDirectory> dataDirectories; // Per-instance data directories (from preset)
    bool dataDirectoriesSnapshotted = false; // True once a snapshot was taken (even an empty one)
    QVariantList dataDirectoriesAsVariant() const;
    void setDataDirectoriesFromVariant(const QVariantList &dirs);
    QString overrideGamePath;
    QStringList overrideFiles;
    QStringList overridePatterns; // Glob patterns for per-user overrides
    QString outputMode = QStringLiteral("physical");  // "physical" or "streaming"
    QString streamResolution = QStringLiteral("1920x1080");
    int streamFps = 60;
    int streamBitrate = 20000;                        // Kbps (20000 = 20 Mbps)
    QString streamCodec = QStringLiteral("h264");     // "h264", "h265", "av1"
    int sunshinePort = 47989;                         // Base port for Sunshine instance
};

Q_DECLARE_METATYPE(InstanceConfig)

/**
 * @brief A complete session profile
 */
struct SessionProfile {
    Q_GADGET
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString layout MEMBER layout)
    Q_PROPERTY(QString gridSubLayout MEMBER gridSubLayout)
    Q_PROPERTY(QString filePath MEMBER filePath)

public:
    QString name;
    QString layout = QStringLiteral("horizontal"); // horizontal, vertical, multi-monitor, grid
    QString gridSubLayout; // "horizontal" (3×1) or "grid-2x2" (2×2 with gap) — only used when layout is "grid"
    QString filePath;
    QList<InstanceConfig> instances;
};

Q_DECLARE_METATYPE(SessionProfile)

/**
 * @brief Manages session profiles - save, load, and current session state
 */
class SessionManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString currentProfileName READ currentProfileName NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentLayout READ currentLayout WRITE setCurrentLayout NOTIFY currentLayoutChanged)
    Q_PROPERTY(QString currentGridSubLayout READ currentGridSubLayout WRITE setCurrentGridSubLayout NOTIFY
                   currentGridSubLayoutChanged)
    Q_PROPERTY(int instanceCount READ instanceCount WRITE setInstanceCount NOTIFY instanceCountChanged)
    Q_PROPERTY(QVariantList savedProfiles READ savedProfilesAsVariant NOTIFY savedProfilesChanged)
    Q_PROPERTY(QVariantList instances READ instancesAsVariant NOTIFY instancesChanged)
    Q_PROPERTY(PresetManager *presetManager READ presetManager WRITE setPresetManager NOTIFY presetManagerChanged)
    Q_PROPERTY(CouchPlayHelperClient *helperClient READ helperClient WRITE setHelperClient NOTIFY helperClientChanged)

public:
    explicit SessionManager(QObject *parent = nullptr);
    ~SessionManager() override;

    PresetManager *presetManager() const
    {
        return m_presetManager;
    }
    void setPresetManager(PresetManager *manager);

    CouchPlayHelperClient *helperClient() const
    {
        return m_helperClient;
    }
    void setHelperClient(CouchPlayHelperClient *client);

    // Profile management
    Q_INVOKABLE bool saveProfile(const QString &name);
    Q_INVOKABLE bool loadProfile(const QString &name);
    Q_INVOKABLE bool deleteProfile(const QString &name);
    Q_INVOKABLE void refreshProfiles();

    // Current session
    Q_INVOKABLE void newSession();
    Q_INVOKABLE QVariantMap getInstanceConfig(int index) const;
    Q_INVOKABLE void setInstanceConfig(int index, const QVariantMap &config);
    Q_INVOKABLE void setInstanceUser(int index, const QString &username);
    Q_INVOKABLE void setInstanceMonitor(int index, int monitor);
    Q_INVOKABLE void setInstanceResolution(int index, int internalW, int internalH, int outputW, int outputH);
    Q_INVOKABLE void setInstanceDevices(int index, const QList<int> &devices);
    /**
     * @brief Set device stable IDs and names for an instance
     *
     * These stable IDs persist across hotplug events and reboots, allowing
     * device assignments to be restored when a profile is loaded.
     * The names list is parallel to stableIds and provides friendly names.
     *
     * @param index Instance index
     * @param stableIds List of device stable IDs
     * @param names List of device friendly names (parallel to stableIds)
     */
    Q_INVOKABLE void setInstanceDeviceStableIds(int index, const QStringList &stableIds, const QStringList &names);
    Q_INVOKABLE void setInstanceGame(int index, const QString &gameCommand);
    Q_INVOKABLE void setInstancePreset(int index, const QString &presetId);
    Q_INVOKABLE void setInstanceDataDirectories(int index, const QVariantList &directories);

    /**
     * @brief Per-player staging folder path for hand-seeded data (configs etc.)
     *
     * Creates the folder (and one subfolder per writable shared directory)
     * if needed and returns the path. Files dropped here are merged into the
     * player's view of the corresponding shared directory at session start.
     * Returns an empty string for invalid indices or instances without a
     * user/preset.
     */
    Q_INVOKABLE QString playerDataFolderPath(int index);

    /**
     * @brief Open the per-player staging folder in the file manager
     *
     * No-op under Flatpak (no host filesystem access) — use
     * playerDataFolderPath() to show a copyable path instead.
     *
     * @return true if an opener was launched
     */
    Q_INVOKABLE bool openPlayerDataFolder(int index);

    Q_INVOKABLE void recalculateOutputResolutions(int screenWidth, int screenHeight);
    Q_INVOKABLE QStringList getAssignedUsers(int excludeIndex) const;

    // Property getters/setters
    QString currentProfileName() const
    {
        return m_currentProfile.name;
    }
    QString currentLayout() const
    {
        return m_currentProfile.layout;
    }
    void setCurrentLayout(const QString &layout);
    QString currentGridSubLayout() const
    {
        return m_currentProfile.gridSubLayout;
    }
    void setCurrentGridSubLayout(const QString &subLayout);
    int instanceCount() const
    {
        return m_currentProfile.instances.size();
    }
    void setInstanceCount(int count);

    QList<SessionProfile> savedProfiles() const
    {
        return m_savedProfiles;
    }
    QVariantList savedProfilesAsVariant() const;
    QVariantList instancesAsVariant() const;

    const SessionProfile &currentProfile() const
    {
        return m_currentProfile;
    }

Q_SIGNALS:
    void currentProfileChanged();
    void currentLayoutChanged();
    void currentGridSubLayoutChanged();
    void instanceCountChanged();
    void savedProfilesChanged();
    void instancesChanged();
    void presetManagerChanged();
    void helperClientChanged();
    void errorOccurred(const QString &message);
    /**
     * @brief Emitted after a profile is successfully loaded
     *
     * This signal allows the UI to trigger device assignment restoration
     * using the stable device IDs saved in the profile.
     *
     * @param deviceInfoByInstance Map of instance index to {stableIds: [...], names: [...]}
     */
    void profileLoaded(const QVariantMap &deviceInfoByInstance);

private:
    QString profilesDir() const;
    QString profilePath(const QString &name) const;

    SessionProfile m_currentProfile;
    QList<SessionProfile> m_savedProfiles;
    PresetManager *m_presetManager = nullptr;
    CouchPlayHelperClient *m_helperClient = nullptr;
};

/**
 * Per-player staging root for hand-seeded data (one subfolder per writable
 * shared directory, named by dataDirectoryStagingSlug()):
 *   <AppConfigLocation>/player-data/<presetId>/<username>/
 */
QString playerDataStagingRoot(const QString &presetId, const QString &username);
