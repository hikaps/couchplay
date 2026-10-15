// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QDBusContext>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QVariantMap>

#include "SystemOps.h"
#include "DirectoryPlan.h"

class UnitMonitor;
class UnitJobMonitor;
class QSocketNotifier;
class QTimer;
class QFileSystemWatcher;

/**
 * CouchPlayHelper - Privileged D-Bus service for split-screen gaming
 *
 * This helper runs as a system service with elevated privileges to perform
 * operations that require root access:
 * - Creating Linux users for secondary players
 * - Enabling systemd linger for user sessions
 * - Setting up Wayland socket ACLs
 * - Changing input device ownership
 *
 * D-Bus interface: io.github.hikaps.CouchPlayHelper
 * Object path: /io/github/hikaps/CouchPlayHelper
 */
class CouchPlayHelper : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.hikaps.CouchPlayHelper")

    friend class UnitMonitor;
    friend class UnitJobMonitor;

public:
    explicit CouchPlayHelper(SystemOps *ops = nullptr, QObject *parent = nullptr);
    ~CouchPlayHelper() override;

public Q_SLOTS:
    /**
     * Create a new Linux user for split-screen gaming
     * Also enables linger for the user automatically and adds to couchplay group
     *
     * @param username Desired username (lowercase, alphanumeric)
     * @param fullName Full name for the user
     * @return UID of created user, or 0 on failure
     */
    uint CreateUser(const QString &username, const QString &fullName);

    /**
     * Delete a CouchPlay user account
     * Only users in the couchplay group can be deleted
     *
     * @param username Username to delete (must be in couchplay group)
     * @param removeHome If true, also delete the user's home directory
     * @return true if successful
     */
    bool DeleteUser(const QString &username, bool removeHome);

    /**
     * Check if a user is in the couchplay group
     *
     * @param username Username to check
     * @return true if user is in couchplay group
     */
    bool IsInCouchPlayGroup(const QString &username);
    /**
     * List all CouchPlay-managed users (couchplay group members).
     * Read-only — no polkit. Each entry is tab-delimited: username\tuid\tgid\thome\tshell.
     *
     * @return list of user entries (filtered: uid 1000-65533, valid shell, home exists)
     */
    QStringList ListCouchPlayUsers();

    /**
     * Get uid/gid/home for a named user.
     * Read-only — no polkit.
     *
     * @param username Username to look up
     * @return map with keys "uid","gid","home", or empty if not found/invalid
     */
    QVariantMap GetUserInfo(const QString &username);

    // Read-only host home paths; no Polkit authorization.
    QVariantMap GetUserHomeInfoByUid(uint uid);

    /**
     * Resolve a target user's existing Steam root on the host.
     *
     * The returned path uses the user's home spelling but has symlinked
     * components below that home removed, so secure helper writes can walk it
     * with O_NOFOLLOW.
     */
    QString GetUserSteamRoot(const QString &username);

    /**
     * Enable systemd linger for a user
     * Required for systemd-run to work properly
     *
     * @param username Username to enable linger for
     * @return true if successful
     */
    bool EnableLinger(const QString &username);

    /**
     * Check if linger is enabled for a user
     *
     * @param username Username to check
     * @return true if linger is enabled
     */
    bool IsLingerEnabled(const QString &username);

    /**
     * Set up runtime directory access for the couchplay group
     *
     * Sets group ACLs on compositor's runtime directory sockets:
     * - XDG_RUNTIME_DIR (traverse)
     * - wayland-0 (rw)
     * - xauth_* (r)
     * - pipewire-0 (rw)
     * - pipewire-0-manager (rw)
     * - pulse/ (traverse)
     * - pulse/native (rw)
     *
     * Called automatically by LaunchInstance() on first invocation.
     * Idempotent - safe to call multiple times.
     *
     * @param compositorUid UID of the compositor user
     * @return true if successful
     */
    bool SetupRuntimeAccess();

    /**
     * Remove runtime directory access for the couchplay group
     *
     * Removes all couchplay group ACLs from compositor's runtime directory.
     * Called automatically on helper shutdown.
     *
     * @param compositorUid UID of the compositor user
     * @return true if successful
     */
    bool RemoveRuntimeAccess();

    /**
     * Change ownership of a device to a specific user
     * Used for input device isolation between instances
     *
     * @param devicePath Path to the device (e.g., /dev/input/event5)
     * @param uid User ID to assign ownership to
     * @return true if successful
     */
    bool ChangeDeviceOwner(const QString &devicePath, uint uid);

    /**
     * Change ownership of multiple devices to a specific user
     *
     * @param devicePaths List of device paths
     * @param uid User ID to assign ownership to
     * @return Number of devices successfully changed
     */
    int ChangeDeviceOwnerBatch(const QStringList &devicePaths, uint uid);

    /**
     * Reset device ownership to root
     *
     * @param devicePath Path to the device
     * @return true if successful
     */
    bool ResetDeviceOwner(const QString &devicePath);

    /**
     * Watch a device for the exit chord without changing its ownership
     * Used for input monitoring in single-user/host sessions
     *
     * @param devicePath Path to the device (e.g., /dev/input/event5)
     * @return true if successful
     */
    bool WatchDevice(const QString &devicePath);



    /**
     * Reset ownership of all managed devices to root
     * Called automatically on helper shutdown
     *
     * @return Number of devices successfully reset
     */
    int ResetAllDevices();

    /**
     * Launch a Gamescope instance in an owned transient systemd unit.
     *
     * The helper writes a protected launch manifest, then starts the installed
     * native helper in a namespace-preserving bootstrap. The returned PID is
     * preparing; instanceReady signals completion of directory preparation.
     *
     * @param username User to run as
     * @param displayContext Owned compositor/display context
     * @param gamescopeArgs Gamescope command-line arguments
     * @param gameCommand Structured child command: executable followed by arguments
     * @param workingDirectory Optional absolute working directory
     * @param sharedRoots Explicit source roots admitted for the working directory
     * @param environment Additional environment variables (VAR=value format)
     * @param privateTemporaryDirectories Whether to isolate the instance's /tmp and /var/tmp
     * @param directoryPreparations Validated per-instance source/mode/target records
     * @return Preparing MainPID, or 0 on failure
     */
    qint64 LaunchInstance(const QString &username,
                           const QString &displayContext,
                           const QStringList &gamescopeArgs,
                           const QStringList &gameCommand,
                           const QString &workingDirectory,
                           const QStringList &sharedRoots,
                           const QStringList &environment,
                           bool privateTemporaryDirectories,
                           const QVariantList &directoryPreparations);

    qint64 LaunchAuxiliaryInstance(const QString &username,
                                   const QString &displayContext,
                                   const QStringList &gamescopeArgs,
                                   const QStringList &gameCommand,
                                   const QString &workingDirectory,
                                   const QStringList &sharedRoots,
                                   const QStringList &environment,
                                   bool privateTemporaryDirectories);

    /**
     * Stop a launched instance
     *
     * @param pid Process ID to stop
     * @return true if the process was successfully signaled
     */
    bool StopInstance(qint64 pid);

    /**
     * Kill a launched instance forcefully
     *
     * @param pid Process ID to kill
     * @return true if the process was successfully signaled
     */
    bool KillInstance(qint64 pid);


    /**
     * Copy a file to a user's directory with proper ownership
     *
     * Used for copying Steam config files (libraryfolders.vdf, shortcuts.vdf)
     * to gaming users' Steam directories.
     *
     * @param sourcePath Source file path
     * @param targetPath Target file path (will be created/overwritten)
     * @param username Target user (file will be owned by this user)
     * @return true if successful
     */
    bool CopyFileToUser(const QString &sourcePath, const QString &targetPath, const QString &username);

    /**
     * Copy a directory to a user's home with proper ownership
     *
     * Copies a source directory into the user's home at the specified relative path.
     * Uses cp -a to preserve ownership, permissions, and symlinks.
     * The targetRelativePath must be relative (no leading /) and must not
     * contain ".." components to prevent escaping the user's home.
     *
     * @param username Target user
     * @param sourceDir Source directory path (must exist and be a directory)
     * @param targetRelativePath Relative path under user's home (e.g., ".local/share/steam")
     * @return true if successful
     */
    bool CopyDirectoryToUser(const QString &username,
                             const QString &sourceDir,
                             const QString &targetRelativePath);

    /**
     * Merge-copy staged files into an existing directory in a user's home
     *
     * Copies the *contents* of sourceDir into the existing target directory
     * (no removal, no nesting — unlike CopyDirectoryToUser's replace
     * semantics). Used to seed per-player staged data (e.g. configs) through
     * an overlay/bind mount so files land in the player's private layer.
     *
     * @param username Target user
     * @param sourceDir Staging directory whose contents are merged
     * @param targetRelativePath Existing relative directory under the user's home
     * @return true if successful
     */
    bool MirrorDirectoryContents(const QString &username,
                                 const QString &sourceDir,
                                 const QString &targetRelativePath);

    /**
     * Create a directory with proper ownership
     *
     * Creates the directory and all parent directories, setting ownership
     * on each created directory.
     *
     * @param path Directory path to create
     * @param username User who should own the directory
     * @return true if successful
     */
    bool CreateUserDirectory(const QString &path, const QString &username);

    /**
     * Set ACLs on a path and all parent directories needed for traversal
     *
     * This is useful for paths on external drives or in /run/media/username/
     * where the user needs rx access to all parent directories to reach the
     * target path. Traversal stops at safe boundaries like /run/media, /mnt,
     * /media, or the filesystem root.
     *
     * @param path Target path to set ACL on
     * @param username User to grant access to
     * @return true if successful
     */
    bool SetPathAclWithParents(const QString &path, const QString &username);

    /**
     * Get a user's Steam user ID
     *
     * Looks in the user's Steam userdata directory to find their Steam ID.
     * This requires root access since other users' home directories are
     * typically not readable.
     *
     * @param username User to get Steam ID for
     * @return Steam user ID string, or empty if not found
     */
    QString GetUserSteamId(const QString &username);

    /**
     * Check whether Steam's first-run setup has completed for a user.
     * Looks for Steam's launch script/binary (steam.sh or ubuntu12_32/steam),
     * not just userdata — a partial or failed bootstrap can leave userdata behind.
     *
     * @param username User to check
     * @return true if Steam's bootstrap content is present
     */
    bool IsSteamBootstrapped(const QString &username);

    /**
     * Write content directly to a file in a user's directory
     *
     * Used for writing generated config files (e.g., shortcuts.vdf) directly
     * to gaming users' directories without needing a temp file.
     *
     * @param content File content as bytes
     * @param targetPath Target file path (will be created/overwritten)
     * @param username Target user (file will be owned by this user)
     * @return true if successful
     */
    bool WriteFileToUser(const QByteArray &content, const QString &targetPath, const QString &username);

    /**
     * Create a virtual Wayland output for streaming capture
     *
     * Launches Gamescope in headless mode as the specified user,
     * creating a capturable virtual display for Sunshine to capture.
     *
     * @param username User to run the virtual display as
     * @param width Output width in pixels
     * @param height Output height in pixels
     * @param refreshRate Refresh rate in Hz
     * @return Wayland socket name (e.g., "wayland-5"), or empty on failure
     */
    QString CreateVirtualOutput(const QString &username, int width, int height, int refreshRate);

    /**
     * Destroy a virtual Wayland output created by CreateVirtualOutput
     *
     * @param username User who owns the virtual display
     * @param waylandSocketName Wayland socket name returned by CreateVirtualOutput
     * @return true if successful
     */
    bool DestroyVirtualOutput(const QString &username, const QString &waylandSocketName);

    /**
     * Create a PipeWire null-sink for audio isolation
     *
     * Creates an isolated audio sink that Sunshine can capture from,
     * preventing audio from mixing between streaming instances.
     *
     * @param username User whose PipeWire instance to create the sink in
     * @param sinkName Name for the null-sink (alphanumeric, hyphens, underscores)
     * @return The sink name, or empty on failure
     */
    QString CreateNullSink(const QString &username, const QString &sinkName);

    /**
     * Destroy a PipeWire null-sink created by CreateNullSink
     *
     * @param username User whose PipeWire instance has the sink
     * @param sinkName Name of the null-sink to remove
     * @return true if successful
     */
    bool DestroyNullSink(const QString &username, const QString &sinkName);

Q_SIGNALS:
    /**
     * Emitted when an owned transient unit exits unexpectedly.
     * Preparation failures include bootstrap status text and errno when available.
     *
     * @param username User whose instance stopped
     * @param pid PID of the stopped process
     * @param reason Failure detail or normal-exit classification
     */
    void instanceStopped(const QString &username, qint64 pid, const QString &reason);
    /** Emitted after the matching instance PID completes bootstrap preparation. */
    void instanceReady(const QString &username, qint64 pid);

    /**
     * Emitted when a gamepad exit chord (Guide/Steam/PS + Start + Select)
     * is held for 2 seconds on any monitored controller.
     */
    void exitChordTriggered();

private:
    struct HidDeviceInfo {
        QString driverPath; // e.g. /sys/bus/hid/drivers/microsoft
        QString deviceId;   // e.g. 0003:045E:028E.0001
    };
    HidDeviceInfo findHidDevice(const QString &devicePath) const;
    QString findHidrawPathForDeviceId(const QString &deviceId) const;
    QString getTempUdevRulePath(const QString &deviceId) const;
    bool writeTempUdevRule(const HidDeviceInfo &hid, const QString &username);
    bool removeTempUdevRule(const QString &deviceId);

    bool checkAuthorization(const QString &action);
    bool isValidDevicePath(const QString &path);
    bool validateUserAndAuth(const QString &username, const QString &action);
    bool runCommand(const QString &program, const QStringList &args, int timeoutMs = 10000);
    bool setupPulseTcpListener(uint compositorUid);
    void removePulseTcpListener(uint compositorUid);
    void restartUserPipeWirePulse(uint compositorUid);

    // Internal helpers (not exposed via D-Bus)
    struct PendingLaunch;
    bool userExists(const QString &username);
    uint getUserUid(const QString &username);
    QString getUserHome(const QString &username);
    QString getUserHomeByUid(uint uid);
    int resetAllDevicesInternal();
    QString generateServiceName(const QString &username);
    QString generateStreamServiceName(const QString &username);
    QString usernameForUnit(const QString &serviceName) const;
    enum class UnitRole { Main, Stream };
    uint callerUid() const;
    void startTransientUnit(const QString &username,
                            const QString &displayContext,
                            const QStringList &gamescopeArgs,
                            const QStringList &gameCommand,
                            const QString &workingDirectory,
                            const QStringList &sharedRoots,
                            const QStringList &environment,
                            bool privateTemporaryDirectories,
                            const QVariantList &directoryPreparations,
                            UnitRole role,
                            const QDBusMessage &request,
                            const QDBusConnection &replyConnection);
    bool stopServiceInstance(const QString &serviceName);
    bool confirmUnitInactive(const QString &serviceName, QString *error = nullptr) const;
    void queryUnitActiveState(const QString &serviceName, quint64 generation,
                              const QDBusObjectPath &unitPath, const QString &reason,
                              bool resolveMissingUnit);
    void monitorUnitState(const QString &serviceName, const QString &username,
                          const QDBusObjectPath &unitPath, bool initialStateIsAuthoritative = false);
    void resolveUnitAfterPropertyError(const QString &serviceName, quint64 generation, const QString &reason);
    void pollUnitMainPid(const QString &serviceName);
    void onUnitReady(const QString &serviceName);
    void onUnitStopped(const QString &serviceName, const QString &reason);
    void noteInitialInactiveUnitState(const QString &serviceName);
    void queryTerminalUnitProperties(const QString &serviceName, quint64 generation);
    void completePendingLaunch(const QString &serviceName, qint64 pid);
    void finalizeStoppedUnit(const QString &serviceName, const QString &reason, bool deferNotification = false);
    void queueStoppedNotification(const QString &username, qint64 pid, const QString &reason);
    void failPendingLaunch(const QString &serviceName, const QString &reason);
    void publishUnitReady(const QString &serviceName);
    void releaseOwnedUnit(const QString &serviceName, bool unlinkManifest);
    bool acquireOverlayLeases(const QString &username, const QList<DirectoryPreparation> &plans,
                              QList<int> *leases, QStringList *leasePaths, QString *error);
    bool reacquireOverlayLeases(const QString &username, const QStringList &leasePaths,
                               QList<int> *leases, QString *error);
    bool
    validateUserPath(const QString &path, const QString &username, const QString &callerName, QStringList &dirsToChown);
    QString findGamescopePath();
    bool unloadNullSinkModule(const QString &username, const QString &sinkName);
    bool isPathWithinAllowedPrefix(const QString &path) const;
    bool pathHasSymlinkComponents(const QString &path, const QString &root);
    bool secureCreateUserDir(const QString &username, const QString &absolutePath);

    QStringList m_modifiedDevices;
    QStringList m_modifiedHidDevices;

    struct VirtualDisplayInfo {
        qint64 pid = 0;
        QString displayContext;
        uint ownerUid = 0;
        uint runtimeUid = 0;
        QString targetUsername;
        QString waylandSocket;
        QString serviceName;



    };
    QMap<QString, VirtualDisplayInfo> m_virtualDisplays;

    struct NullSinkInfo {
        int moduleIndex;
        QString sinkName;
    };
    QMap<QString, QList<NullSinkInfo>> m_nullSinks;


    // Track launched transient units
    QMap<QString, QString> m_usernameToUnitName; // username -> main service name
    QMap<QString, QString> m_auxiliaryUnitNameForUsername; // username -> auxiliary service name
    QMap<QString, QString> m_unitUsernameByName; // service name -> username
    QMap<QString, UnitRole> m_unitRoleByName;
    QMap<QString, quint64> m_unitGenerationByName;
    quint64 m_nextUnitGeneration = 0;
    QMap<qint64, QString> m_pidToUsername; // PID -> username (reverse lookup for Stop/Kill)
    QMap<qint64, QString> m_pidToUnitName;
    QMap<QString, quint64> m_pidPollGenerationByUnit;
    QHash<qint64, uint> m_pidOwnerUid;
    QHash<QString, uint> m_unitOwnerUid;
    struct StoppedInstanceNotification { QString username; qint64 pid = 0; QString reason; };
    QList<StoppedInstanceNotification> m_deferredStoppedNotifications;
    struct TargetIdentity { uid_t uid = 0; gid_t gid = 0; QString declaredHome; QString canonicalHome; };
    QMap<QString, TargetIdentity> m_targetIdentityByUnit;
    QHash<QString, uint> m_compositorUidForUsername; // username -> compositor UID
    QMap<QString, QString> m_launchTokenByUnitName;
    QMap<QString, QDBusObjectPath> m_unitPathForUnit;
    QSet<QString> m_readyUnitNames;
    QMap<QString, QList<int>> m_overlayLeaseFdsByUnit;
    QMap<QString, QStringList> m_overlayLeasePathsByUnit;
    QMap<QString, PendingLaunch *> m_pendingLaunches;
    QSet<QString> m_announcedReadyUnitNames;
    QSet<QString> m_terminalStateQueries;
    QSet<QString> m_initialInactiveUnitNames;
    // Units being explicitly stopped (suppresses crash detection)
    QSet<QString> m_stoppingUnits;

    // Per-unit D-Bus monitors for crash detection
    QMap<QString, UnitMonitor *> m_monitors;
    QMap<QString, UnitJobMonitor *> m_jobMonitors;
    QHash<QString, QString> m_failedJobResults;
    // Track which compositor UIDs have runtime access set up
    QSet<uint> m_runtimeAccessSetForUid;

    // System operations abstraction (for testing/mocking)
    SystemOps *m_ops;

    QString m_stateFilePath;
    bool m_reconcilingState = false;
    bool m_recoverySaveRequested = false;
    QSet<QString> m_deferredReleaseUnits;
    QJsonObject m_legacyActiveMounts;
    QSet<QString> m_cleanedLaunchManifestTokens;
    bool saveState();
    bool subscribeSystemdManager(QString *error = nullptr) const;
    void loadAndReconcileState();
    bool reconcileLegacyMounts(const QSet<QString> &activeSystemdUnits,
                               const QSet<QString> &stoppedUnitNames,
                               bool systemdStateKnown);
    bool cleanupCopyScratchForUnit(const QString &serviceName, const QString &token) const;
    bool readLaunchManifestForUnit(const QString &serviceName, const QString &token,
                                   LaunchManifest *manifest, QString *error = nullptr) const;
    void removeRuntimeAcls(const QString &runtimeDir);
    void cleanupTcpListenerIfLast(const QString &username);
    void setupUinputAccess();
    void removeUinputAccess();

    struct WatchedDevice {
        int fd;
        QSocketNotifier *notifier = nullptr;
        bool startPressed = false;
        bool selectPressed = false;
        QTimer *chordTimer = nullptr;
        bool isHidrawDevice = false; // true for /dev/hidrawN; uses raw HID packet parsing
        bool isSteamController = false; // true if it is a Valve Steam Controller / Puck device
        uint8_t lastB8 = 0;
        uint8_t lastB9 = 0;
        QSet<int> seenSizes;
    };

    QMap<QString, WatchedDevice *> m_watchedDevices;

    void startWatchingDevice(const QString &devicePath);
    void stopWatchingDevice(const QString &devicePath);
    void stopWatchingAllDevices();
    void onDeviceDataAvailable(const QString &devicePath);
    void onExitChordTimeout(const QString &devicePath);

    QFileSystemWatcher *m_inputWatcher = nullptr;
    QTimer *m_inputDebounceTimer = nullptr;
    QSet<int> m_knownEventNumbers;

    void onInputDirectoryChanged();
    void onInputDebounceTimeout();
    void checkForNewVirtualDevices();
    bool isVirtualDevice(int eventNumber);
    QString getDeviceName(int eventNumber) const;
};