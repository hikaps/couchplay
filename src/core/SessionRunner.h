// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QList>
#include <functional>
#include <QElapsedTimer>
#include <QMap>
#include <QProcess>
#include <QObject>
#include <qqmlintegration.h>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include "LaunchTypes.h"
#include "SessionManager.h"

#include "PadForwarder.h"
#include "PlayerStore.h"
#include "SandboxCommand.h"
#include "HeroicConfigManager.h"
#include "SteamConfigManager.h"
#include "StreamManager.h"
#include "StreamAudio.h"

class QAction;
class QScreen;

class GamescopeInstance;
class DeviceManager;
class SessionManager;
class SettingsManager;
class MonitorManager;
class StreamManager;
class VirtualDisplayManager;
class WindowManager;
class PresetManager;

/**
 * @brief Orchestrates running a complete split-screen gaming session
 *
 * SessionRunner manages the lifecycle of multiple GamescopeInstance objects,
 * handles window layout calculations, device ownership transfers, and
 * coordinates with the SessionManager for configuration.
 */
class SessionRunner : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(int runningInstanceCount READ runningInstanceCount NOTIFY runningInstanceCountChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(QVariantList instances READ instancesAsVariant NOTIFY instancesChanged)

    // Dependencies
    Q_PROPERTY(SessionManager *sessionManager READ sessionManager WRITE setSessionManager NOTIFY sessionManagerChanged)
    Q_PROPERTY(DeviceManager *deviceManager READ deviceManager WRITE setDeviceManager NOTIFY deviceManagerChanged)
    Q_PROPERTY(PlayerStore *playerStore READ playerStore WRITE setPlayerStore NOTIFY playerStoreChanged)
    Q_PROPERTY(PresetManager *presetManager READ presetManager WRITE setPresetManager NOTIFY presetManagerChanged)
    Q_PROPERTY(SteamConfigManager *steamConfigManager READ steamConfigManager WRITE setSteamConfigManager NOTIFY
                   steamConfigManagerChanged)
    Q_PROPERTY(HeroicConfigManager *heroicConfigManager READ heroicConfigManager WRITE setHeroicConfigManager NOTIFY
                   heroicConfigManagerChanged)
    Q_PROPERTY(
        SettingsManager *settingsManager READ settingsManager WRITE setSettingsManager NOTIFY settingsManagerChanged)
    Q_PROPERTY(MonitorManager *monitorManager READ monitorManager WRITE setMonitorManager NOTIFY monitorManagerChanged)
    Q_PROPERTY(StreamManager *streamManager READ streamManager CONSTANT)
    Q_PROPERTY(bool skipPairingCheck READ skipPairingCheck WRITE setSkipPairingCheck NOTIFY skipPairingCheckChanged)
    Q_PROPERTY(QVariantMap gameOverride READ gameOverride WRITE setGameOverride NOTIFY gameOverrideChanged)
    Q_PROPERTY(QString launcherOverride READ launcherOverride WRITE setLauncherOverride NOTIFY launcherOverrideChanged)

public:
    explicit SessionRunner(QObject *parent = nullptr);
    ~SessionRunner() override;

    /**
     * @brief Start all instances in the current session
     * @return true if all instances started successfully
     */
    Q_INVOKABLE bool start();

    /**
     * @brief start() after the user chose to go on despite pending host tool updates (see staleToolsDetected)
     */
    Q_INVOKABLE bool startIgnoringStaleTools();

    /**
     * @brief Stop all running instances
     */
    Q_INVOKABLE void stop();

    /**
     * @brief Stop a specific instance by index
     * @param index Instance index
     */
    Q_INVOKABLE void stopInstance(int index);

    /**
     * @brief Check if any instance is running
     */
    bool isRunning() const;
    bool isActive() const
    {
        return m_active;
    }

    /**
     * @brief Get the number of currently running instances
     */
    int runningInstanceCount() const;

    /**
     * @brief Instance index of the streaming player assigned to this gaming user, or -1
     *
     * Only players with a stream in the current session count; an empty username never matches.
     */
    Q_INVOKABLE int streamingInstanceForUser(const QString &username) const;

    /// Number of Moonlight clients paired with the gaming user's Sunshine identity
    Q_INVOKABLE int pairedClientCount(const QString &username) const;

    /**
     * @brief Get current status message
     */
    QString status() const
    {
        return m_status;
    }

    /**
     * @brief Get list of instances with their status
     */
    QVariantList instancesAsVariant() const;

    // Dependency getters/setters
    SessionManager *sessionManager() const
    {
        return m_sessionManager;
    }
    void setSessionManager(SessionManager *manager);

    DeviceManager *deviceManager() const
    {
        return m_deviceManager;
    }
    StreamManager *streamManager() const
    {
        return m_streamManager;
    }
    void setDeviceManager(DeviceManager *manager);

    PlayerStore *playerStore() const
    {
        return m_playerStore;
    }
    void setPlayerStore(PlayerStore *store);
    /** Test seams: replace how sandbox instances are created and how host tools are located. */
    void setInstanceFactory(std::function<GamescopeInstance *(QObject *)> factory)
    {
        m_instanceFactory = std::move(factory);
    }
    void setToolFinderForTesting(std::function<QString(const QString &)> finder)
    {
        m_toolFinder = std::move(finder);
    }

    PresetManager *presetManager() const
    {
        return m_presetManager;
    }
    void setPresetManager(PresetManager *manager);

    SteamConfigManager *steamConfigManager() const
    {
        return m_steamConfigManager;
    }
    void setSteamConfigManager(SteamConfigManager *manager);

    HeroicConfigManager *heroicConfigManager() const
    {
        return m_heroicConfigManager;
    }
    void setHeroicConfigManager(HeroicConfigManager *manager);

    SettingsManager *settingsManager() const
    {
        return m_settingsManager;
    }
    void setSettingsManager(SettingsManager *manager);

    /** Source of each output's real scale; without it gamescope gets logical sizes. */
    MonitorManager *monitorManager() const
    {
        return m_monitorManager;
    }
    void setMonitorManager(MonitorManager *manager);

    /** When true, start() does not require a paired Moonlight client for streaming players (--skip-pairing-check). */
    bool skipPairingCheck() const
    {
        return m_skipPairingCheck;
    }
    void setSkipPairingCheck(bool skip);

    /** --game: replaces the game of players whose launcher matches, for the next start() only; never saved. */
    QVariantMap gameOverride() const
    {
        return m_gameOverride.toVariant();
    }
    void setGameOverride(const QVariantMap &selection);

    /** --launcher: launcher for players without one, for the next start() only; never saved. */
    QString launcherOverride() const
    {
        return m_launcherOverride;
    }
    void setLauncherOverride(const QString &launcher);

    /**
     * Fills players without a launcher from @p launcher (preset id, else exact preset name, case-insensitive) or,
     * without it, from @p game's launcher (built-in preset ids equal launcher ids), then gives @p game to every
     * player whose launcher matches it. Returns an error message, or an empty string on success.
     */
    static QString applyLaunchOverrides(SessionProfile &profile, const QString &launcher, const GameSelection &game,
                                        const PresetManager &presets);

    /**
     * @brief Calculate window geometries for a given layout
     * @param layout Layout type: "horizontal", "vertical", "grid"
     * @param instanceCount Number of instances
     * @param screenGeometry Available screen geometry
     * @param gridSubLayout For "grid" layout with 3 players: "horizontal" (3×1), "grid-2x2" (2×2 with gap), or empty
     * (defaults to horizontal)
     * @return List of QRect geometries for each instance
     */
    static QList<QRect> calculateLayout(const QString &layout,
                                        int instanceCount,
                                        const QRect &screenGeometry,
                                        const QString &gridSubLayout = QString());

    /**
     * Parse a per-instance render resolution such as "1280x720".
     * @return the size, or an invalid QSize if empty, malformed, or outside 320x200..7680x4320
     */
    static QSize parseRenderResolution(const QString &text);

    /** The connected screen whose QScreen::name() equals the connector, or nullptr. */
    static QScreen *screenByConnector(const QString &connector);

    /**
     * Check the per-player display choices of a profile against the connected screens.
     * @return an empty string when valid, otherwise the user-facing error
     */
    static QString validateDisplays(const SessionProfile &profile);

    /** Same check against an explicit set of connected connectors (testable without a real display). */
    static QString validateDisplays(const SessionProfile &profile,
                                    const QStringList &connectors,
                                    const QString &primaryConnector);

    /**
     * Every streaming player with a gaming user needs at least one paired Moonlight client.
     * @return an empty string when satisfied, otherwise the user-facing error
     */
    static QString validatePairing(const SessionProfile &profile);

    /** Same check with an explicit paired-client count lookup (testable without disk state). */
    static QString validatePairing(const SessionProfile &profile,
                                   const std::function<int(const QString &username)> &pairedCount);

Q_SIGNALS:
    void runningChanged();
    void runningInstanceCountChanged();
    void statusChanged();
    void instancesChanged();
    void sessionManagerChanged();
    void deviceManagerChanged();
    void playerStoreChanged();
    void presetManagerChanged();
    void steamConfigManagerChanged();
    void heroicConfigManagerChanged();
    void settingsManagerChanged();
    void monitorManagerChanged();
    void skipPairingCheckChanged();
    void gameOverrideChanged();
    void launcherOverrideChanged();
    void errorOccurred(const QString &message);
    // A Steam tool on the host has an update pending, so start() declined to begin; startIgnoringStaleTools() goes ahead.
    void staleToolsDetected(const QStringList &tools);
    // Library sharing was skipped for a player whose Steam has never run; it applies from their next session.
    void librarySharingDeferred(const QString &username);
    void activeChanged();
    void sessionStartFailed(const QString &message);
    void sessionStarted();
    void sessionStopped();
    void instanceStarted(int index);
    void instanceStopped(int index);

private Q_SLOTS:
    void onInstanceStarted();
    void onInstanceStopped();
    void onInstanceError(const QString &message);
    void onWindowPositioned(int requestId, const QString &windowId);
    void onWindowPositioningTimeout(int requestId);
    void onDeviceReconnected(const QString &stableId, int eventNumber, int instanceIndex);
    void onDeviceAdded(int eventNumber, const QString &name);
    void startNextInstance();
    void onHookFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onHookError(QProcess::ProcessError error);

private:
    void setStatus(const QString &status);
    void cleanupInstances();
    void continueStart();
    void runHook(const QString &path, bool postHook);
    void beginFinalization(bool startupFailure, const QString &message = QString());
    void finishFinalization();
    void setActive(bool active);
    const SessionProfile &activeProfile() const;
    bool startPadForwarders();
    void stopPadForwarders();
    QString missingHostToolError() const;
    bool setupSessionResources();
    void teardownSharingState();
    /** The screen the main-screen players split: the profile's choice, else the primary screen. */
    static QScreen *mainScreen(const SessionProfile &profile);
    void positionInstanceWindow(GamescopeInstance *instance);
    void setupGlobalShortcut();
    void inhibitScreenSaver();
    void uninhibitScreenSaver();
    void teardownStreamingInstances();
    void cleanupStreamingInstance(int index);
    void continueFinalizationAfterStopping();
    bool grantDeviceToInstance(int eventNumber, int instanceIndex);
    qint64 nowMs() const;
    void resetGamepadAttribution();
    bool hasOwnedPids() const;

    struct SessionRuntimeSnapshot {
        SessionProfile profile;
        QList<LaunchPreset> presets;
        QList<LaunchCommand> launchCommands;
        bool syncSteamShortcuts = false;
        bool shareSteamLibraries = false;
        bool syncHeroicShortcuts = false;
    };

    struct StreamingInstanceInfo {
        QString outputName; // KWin output Sunshine captures
        QString sinkName;   // Per-player stream sink
    };

    void startVirtualDisplays();
    void continueStartAfterDisplays();
    void proceedAfterInstanceStarted(GamescopeInstance *instance);

    QMap<int, QList<SandboxMount>> m_instanceMounts;
    uint m_screenSaverCookie = 0;


    QList<qint64> getGamescopePids() const;

    SessionManager *m_sessionManager = nullptr;
    SessionRuntimeSnapshot m_startingSnapshot;
    bool m_hasStartingProfile = false;
    DeviceManager *m_deviceManager = nullptr;
    PresetManager *m_presetManager = nullptr;
    SteamConfigManager *m_steamConfigManager = nullptr;
    HeroicConfigManager *m_heroicConfigManager = nullptr;
    SettingsManager *m_settingsManager = nullptr;
    MonitorManager *m_monitorManager = nullptr;
    WindowManager *m_windowManager = nullptr;
    StreamManager *m_streamManager = nullptr;
    VirtualDisplayManager *m_virtualDisplays = nullptr;
    bool m_skipPairingCheck = false;
    // Set by startIgnoringStaleTools(): the user already saw the pending host tool updates.
    bool m_ignoreStaleTools = false;
    GameSelection m_gameOverride; // --game: replaces the game of players whose launcher matches, for the next start() only
    QString m_launcherOverride; // --launcher: preset id or name for players without a launcher, for the next start() only
    StreamAudio m_streamAudio;
    QSet<int> m_pendingDisplays;
    QHash<int, QString> m_virtualOutputNames;
    QHash<int, QRect> m_virtualRects;
    QList<GamescopeInstance *> m_instances;
    QAction *m_stopAction = nullptr;
    QString m_status;
    bool m_active = false;
    bool m_sessionStartupComplete = false;
    bool m_finalizing = false;
    bool m_startupFailure = false;
    bool m_finalizationCleanupStarted = false;
    bool m_stoppingInstances = false;
    bool m_launchErrorReported = false;
    bool m_streamStartupErrorReported = false;
    bool m_preHookCompleted = false;
    bool m_postHookArmed = false;
    bool m_hookIsPost = false;
    bool m_postHookStarted = false;
    QString m_finalizationMessage;
    QProcess *m_hookProcess = nullptr;
    QMap<int, PadForwarder *> m_padForwarders; // One virtual pad per instance
    QMap<int, QStringList> m_directDevices;    // Keyboards/mice bound straight into each sandbox
    PlayerStore *m_playerStore = nullptr;
    std::function<GamescopeInstance *(QObject *)> m_instanceFactory;
    std::function<QString(const QString &)> m_toolFinder;
    QStringList m_positionedWindowIds; // Window IDs we've positioned (for excluding)

    // Sequential instance launching state
    int m_nextInstanceToStart = 0;
    QList<QVariantMap> m_pendingInstanceConfigs;
    QList<QRect> m_layouts; // logical
    QStringList m_layoutOutputs; // output each layout lies on
    QMap<int, StreamingInstanceInfo> m_streamingInstances;

    // Moonlight gamepad attribution (Sunshine uhid devices -> streaming player)
    struct GamepadClaim {
        int instanceIndex = -1;
        qint64 atMs = 0;
    };
    struct SunshineAttribution {
        QString uniq;
        int instanceIndex = -1;
        qint64 atMs = 0;
    };
    QList<GamepadClaim> m_gamepadClaims;
    QList<SunshineAttribution> m_sunshineAttributions;
    qint64 m_claimTtlMs = 5000;
    qint64 m_uniqWindowMs = 3000;
    qint64 m_clockOverrideMs = -1; // test seam: >= 0 replaces the monotonic clock
    QElapsedTimer m_clock;

    // Privileged sharing state (mounts, Steam library sharing) is active from
    // setup until teardown — natural instance exit must release it too
    bool m_sharedStateActive = false;
    QSet<QString> m_steamSharedUsers;
};
