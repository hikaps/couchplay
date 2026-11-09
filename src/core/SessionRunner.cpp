// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SessionRunner.h"
#include "../../helper/HomePathMapping.h"
#include "../../helper/DirectoryPlan.h"
#include "../dbus/CouchPlayHelperClient.h"
#include "DeviceManager.h"
#include "GamescopeInstance.h"
#include "HostProcess.h"
#include "Logging.h"
#include "PresetManager.h"
#include "SessionManager.h"
#include "SettingsManager.h"
#include "SteamConfigManager.h"
#include "StreamManager.h"
#include "SunshineConfig.h"
#include "UserLookup.h"
#include "WindowManager.h"
#include "VirtualDisplayManager.h"

#include <QAction>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
#include <QScopedValueRollback>
#include <QSet>
#include <QFileInfo>

#include <KGlobalAccel>
#include <KLocalizedString>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

static const QString COUCHPLAY_GROUP = QStringLiteral("couchplay");

static bool isUserInCouchPlayGroup(const QString &username)
{
    struct group *grp = getgrnam(COUCHPLAY_GROUP.toLocal8Bit().constData());
    if (!grp) {
        return false;
    }

    for (char **member = grp->gr_mem; *member != nullptr; ++member) {
        if (username == QString::fromLocal8Bit(*member)) {
            return true;
        }
    }

    // Primary group check: getgrnam(3) only returns supplementary members in gr_mem
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    if (pw && pw->pw_gid == grp->gr_gid) {
        return true;
    }

    return false;
}

SessionRunner::SessionRunner(QObject *parent)
    : QObject(parent)
    , m_windowManager(new WindowManager(this))
{
    setStatus(QStringLiteral("Ready"));
    setupGlobalShortcut();



    connect(m_windowManager, &WindowManager::gamescopeWindowPositioned, this, &SessionRunner::onWindowPositioned);
    connect(m_windowManager, &WindowManager::positioningTimedOut, this, &SessionRunner::onWindowPositioningTimeout);

    m_virtualDisplays = new VirtualDisplayManager(this);
    connect(m_virtualDisplays,
            &VirtualDisplayManager::ready,
            this,
            [this](int index, const QString &outputName, const QRect &geometry) {
                if (!m_pendingDisplays.remove(index) || m_finalizing) {
                    return;
                }
                m_virtualOutputNames[index] = outputName;
                m_virtualRects[index] = geometry;
                if (m_pendingDisplays.isEmpty()) {
                    continueStartAfterDisplays();
                }
            });
    connect(m_virtualDisplays, &VirtualDisplayManager::failed, this, [this](int index, const QString &message) {
        if (m_finalizing || !m_pendingDisplays.contains(index)) {
            return;
        }
        m_pendingDisplays.clear();
        beginFinalization(true,
                          message.startsWith(QStringLiteral("couchplay-vdisplay is not installed"))
                              ? message
                              : QStringLiteral("Player %1: could not create virtual display: %2")
                                    .arg(index + 1)
                                    .arg(message));
    });

    m_streamManager = new StreamManager(this);
    connect(m_streamManager, &StreamManager::streamError,
            this, [this](int instanceIndex, const QString &error) {
        const QString message = QStringLiteral("Stream %1: %2").arg(instanceIndex).arg(error);
        Q_EMIT errorOccurred(message);
        if (!m_sessionStartupComplete && !m_finalizing
            && instanceIndex == m_nextInstanceToStart
            && m_streamingInstances.contains(instanceIndex)) {
            m_streamStartupErrorReported = true;
            beginFinalization(true, message);
        }
    });
    connect(m_streamManager, &StreamManager::streamStarted, this, [this](int instanceIndex) {
        if (m_finalizing || m_sessionStartupComplete || instanceIndex != m_nextInstanceToStart
            || !m_streamingInstances.contains(instanceIndex)) {
            return;
        }
        for (GamescopeInstance *instance : std::as_const(m_instances)) {
            if (instance->index() == instanceIndex) {
                proceedAfterInstanceStarted(instance);
                break;
            }
        }
    });
    connect(m_streamManager, &StreamManager::streamStopped, this, [this](int instanceIndex) {
        cleanupStreamingInstance(instanceIndex);
        QMetaObject::invokeMethod(this, [this]() {
            if (m_finalizing) {
                continueFinalizationAfterStopping();
            }
        }, Qt::QueuedConnection);
    });
    connect(m_streamManager, &StreamManager::gamepadAnnounced, this, [this](int instanceIndex) {
        if (!m_active || m_finalizing || !m_streamingInstances.contains(instanceIndex)) {
            return;
        }
        m_gamepadClaims.append({instanceIndex, nowMs()});
    });
    m_clock.start();
}
SessionRunner::~SessionRunner()
{
    m_streamManager->stopAll();
    stop();
}

void SessionRunner::setStatus(const QString &status)
{
    if (m_status != status) {
        m_status = status;
        Q_EMIT statusChanged();
    }
}

void SessionRunner::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged();
}

int SessionRunner::pairedClientCount(const QString &username) const
{
    return SunshineConfig::pairedClientCount(username);
}

int SessionRunner::streamingInstanceForUser(const QString &username) const
{
    if (username.isEmpty() || (!m_hasStartingProfile && !m_sessionManager)) {
        return -1;
    }
    const QList<InstanceConfig> &instances = activeProfile().instances;
    for (auto it = m_streamingInstances.constBegin(); it != m_streamingInstances.constEnd(); ++it) {
        if (it.key() >= 0 && it.key() < instances.size() && instances[it.key()].username == username) {
            return it.key();
        }
    }
    return -1;
}

const SessionProfile &SessionRunner::activeProfile() const
{
    return m_hasStartingProfile ? m_startingSnapshot.profile : m_sessionManager->currentProfile();
}


void SessionRunner::runHook(const QString &path, bool postHook)
{
    m_hookIsPost = postHook;
    const bool isFlatpak = HostProcess::runningInFlatpak();
    if (path.isEmpty()) {
        if (postHook) {
            finishFinalization();
        } else {
            m_preHookCompleted = true;
            continueStart();
        }
        return;
    }
    if (!QDir::isAbsolutePath(path) || (!isFlatpak && !QFileInfo(path).isExecutable())) {
        const QString message = QStringLiteral("%1-session script is not executable: %2")
            .arg(postHook ? QStringLiteral("Post") : QStringLiteral("Pre"), path);
        if (postHook) {
            Q_EMIT errorOccurred(message);
            finishFinalization();
        } else {
            beginFinalization(true, message);
        }
        return;
    }

    m_hookProcess = new QProcess(this);
    m_hookProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_hookProcess, &QProcess::finished, this, &SessionRunner::onHookFinished);
    connect(m_hookProcess, &QProcess::errorOccurred, this, &SessionRunner::onHookError);

    HostProcess::configure(*m_hookProcess, path, {});
    m_hookProcess->start();
}

void SessionRunner::onHookFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_hookProcess) {
        return;
    }
    const QByteArray output = m_hookProcess->readAll();
    if (!output.isEmpty()) {
        qCDebug(couchplayCore) << "Session hook output:" << output.trimmed();
    }
    const bool postHook = m_hookIsPost;
    const QString program = m_hookProcess->program();
    m_hookProcess->deleteLater();
    m_hookProcess = nullptr;

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        const QString message = QStringLiteral("%1-session script exited with code %2: %3")
            .arg(postHook ? QStringLiteral("Post") : QStringLiteral("Pre"))
            .arg(exitCode)
            .arg(program);
        if (postHook) {
            Q_EMIT errorOccurred(message);
            finishFinalization();
        } else {
            beginFinalization(true, message);
        }
        return;
    }

    if (postHook) {
        finishFinalization();
    } else {
        m_preHookCompleted = true;
        continueStart();
    }
}

void SessionRunner::onHookError(QProcess::ProcessError error)
{
    if (!m_hookProcess || error != QProcess::FailedToStart) {
        return;
    }
    const bool postHook = m_hookIsPost;
    const QString program = m_hookProcess->program();
    m_hookProcess->deleteLater();
    m_hookProcess = nullptr;
    const QString message = QStringLiteral("%1-session script failed to start: %2")
        .arg(postHook ? QStringLiteral("Post") : QStringLiteral("Pre"), program);
    if (postHook) {
        Q_EMIT errorOccurred(message);
        finishFinalization();
    } else {
        beginFinalization(true, message);
    }
}

void SessionRunner::setSessionManager(SessionManager *manager)
{
    if (m_sessionManager != manager) {
        m_sessionManager = manager;
        Q_EMIT sessionManagerChanged();
    }
}

void SessionRunner::setDeviceManager(DeviceManager *manager)
{
    if (m_deviceManager != manager) {
        if (m_deviceManager) {
            disconnect(m_deviceManager, &DeviceManager::deviceReconnected, this, &SessionRunner::onDeviceReconnected);
            disconnect(m_deviceManager, &DeviceManager::deviceAdded, this, &SessionRunner::onDeviceAdded);
        }

        m_deviceManager = manager;

        if (m_deviceManager) {
            connect(m_deviceManager, &DeviceManager::deviceReconnected, this, &SessionRunner::onDeviceReconnected);
            connect(m_deviceManager, &DeviceManager::deviceAdded, this, &SessionRunner::onDeviceAdded);
        }

        Q_EMIT deviceManagerChanged();
    }
}

void SessionRunner::setHelperClient(CouchPlayHelperClient *client)
{
    if (m_helperClient != client) {
        if (m_helperClient) {
            disconnect(m_helperClient, &CouchPlayHelperClient::exitChordTriggered, this, &SessionRunner::stop);
        }
        m_helperClient = client;
        if (m_helperClient) {
            connect(m_helperClient, &CouchPlayHelperClient::exitChordTriggered, this, &SessionRunner::stop);
        }
        Q_EMIT helperClientChanged();
    }
}

void SessionRunner::setPresetManager(PresetManager *manager)
{
    if (m_presetManager != manager) {
        m_presetManager = manager;
        Q_EMIT presetManagerChanged();
    }
}

void SessionRunner::setSteamConfigManager(SteamConfigManager *manager)
{
    if (m_steamConfigManager != manager) {
        m_steamConfigManager = manager;
        Q_EMIT steamConfigManagerChanged();
    }
}

void SessionRunner::setHeroicConfigManager(HeroicConfigManager *manager)
{
    if (m_heroicConfigManager != manager) {
        m_heroicConfigManager = manager;
        Q_EMIT heroicConfigManagerChanged();
    }
}

void SessionRunner::setSettingsManager(SettingsManager *manager)
{
    if (m_settingsManager != manager) {
        m_settingsManager = manager;
        Q_EMIT settingsManagerChanged();
    }
}

void SessionRunner::setSkipPairingCheck(bool skip)
{
    if (m_skipPairingCheck != skip) {
        m_skipPairingCheck = skip;
        Q_EMIT skipPairingCheckChanged();
    }
}

bool SessionRunner::start()
{
    if (!m_sessionManager) {
        Q_EMIT errorOccurred(QStringLiteral("No session manager configured"));
        return false;
    }
    if (m_active) {
        Q_EMIT errorOccurred(QStringLiteral("Session already running"));
        return false;
    }

    cleanupInstances();
    resetGamepadAttribution();
    m_sessionStartupComplete = false;
    m_finalizing = false;
    m_startupFailure = false;
    m_preHookCompleted = false;
    m_postHookArmed = false;
    m_postHookStarted = false;
    m_launchErrorReported = false;
    m_streamStartupErrorReported = false;
    m_finalizationMessage.clear();
    m_startingSnapshot = {};
    m_hasStartingProfile = false;

    const SessionProfile profile = m_sessionManager->currentProfile();
    const int instanceCount = profile.instances.size();
    if (instanceCount < 1) {
        Q_EMIT errorOccurred(QStringLiteral("No instances configured"));
        setStatus(QStringLiteral("Error"));
        return false;
    }
    if (!m_presetManager) {
        Q_EMIT errorOccurred(QStringLiteral("No preset manager configured"));
        setStatus(QStringLiteral("Error"));
        return false;
    }
    const QString displayError = validateDisplays(profile);
    if (!displayError.isEmpty()) {
        Q_EMIT errorOccurred(displayError);
        setStatus(QStringLiteral("Error"));
        return false;
    }
    if (!m_skipPairingCheck) {
        const QString pairingError = validatePairing(profile);
        if (!pairingError.isEmpty()) {
            Q_EMIT errorOccurred(pairingError);
            setStatus(QStringLiteral("Error"));
            return false;
        }
    }
    m_startingSnapshot.profile = profile;
    m_startingSnapshot.syncSteamShortcuts = m_steamConfigManager && m_steamConfigManager->syncShortcutsEnabled();
    m_startingSnapshot.shareSteamLibraries = m_steamConfigManager && m_steamConfigManager->shareLibraryEnabled();
    m_startingSnapshot.syncHeroicShortcuts = m_heroicConfigManager && m_heroicConfigManager->syncShortcutsEnabled();
    for (int i = 0; i < instanceCount; ++i) {
        const QString presetId = profile.instances[i].presetId.isEmpty()
            ? QStringLiteral("steam") : profile.instances[i].presetId;
        const LaunchPreset preset = m_presetManager->getPreset(presetId);
        if (preset.id.isEmpty()) {
            Q_EMIT errorOccurred(QStringLiteral("Launch preset not found: %1").arg(presetId));
            setStatus(QStringLiteral("Error"));
            m_startingSnapshot = {};
            return false;
        }
        const LaunchCommand command = m_presetManager->buildLaunchCommand(preset, profile.instances[i].gameSelection);
        if (!command.isValid()) {
            Q_EMIT errorOccurred(QStringLiteral("Invalid launch command for player %1: %2")
                                     .arg(i + 1)
                                     .arg(command.errorMessage));
            setStatus(QStringLiteral("Error"));
            m_startingSnapshot = {};
            return false;
        }
        m_startingSnapshot.presets.append(preset);
        m_startingSnapshot.launchCommands.append(command);
    }


    QSet<QString> usedUsers;
    for (int i = 0; i < instanceCount; ++i) {
        const QString &username = profile.instances[i].username;
        if (username.isEmpty()) {
            continue;
        }
        if (usedUsers.contains(username)) {
            Q_EMIT errorOccurred(
                QStringLiteral("User '%1' is assigned to multiple instances. Each instance needs a unique user.")
                    .arg(username));
            setStatus(QStringLiteral("Error"));
            return false;
        }
        usedUsers.insert(username);
    }

    QString compositorUser = qEnvironmentVariable("USER");
    if (compositorUser.isEmpty()) {
        struct passwd *compositorPw = getpwuid(getuid());
        compositorUser = compositorPw ? QString::fromLocal8Bit(compositorPw->pw_name) : QString();
    }
    for (int i = 0; i < instanceCount; ++i) {
        const QString &username = profile.instances[i].username;
        if (username.isEmpty() || username == compositorUser) {
            continue;
        }
        const bool inGroup = (m_helperClient && m_helperClient->isAvailable())
            ? m_helperClient->isInCouchPlayGroup(username)
            : isUserInCouchPlayGroup(username);
        if (!inGroup) {
            Q_EMIT errorOccurred(QStringLiteral("User '%1' is not a CouchPlay managed user. Please create the user via "
                                                "CouchPlay or add them to the 'couchplay' group.")
                                     .arg(username));
            setStatus(QStringLiteral("Error"));
            return false;
        }
    }

    bool steamRequired = false;
    bool steamShortcutsNeeded = false;
    for (int i = 0; i < instanceCount; ++i) {
        const InstanceConfig &instance = profile.instances[i];
        const bool selectedShortcut = instance.gameSelection.launcherId == QStringLiteral("steam")
            && instance.gameSelection.backend == QStringLiteral("shortcut");
        const bool requiresSteam = m_startingSnapshot.presets[i].requiredIntegrations.contains(QStringLiteral("steam"))
            || selectedShortcut;
        steamRequired |= requiresSteam;
        steamShortcutsNeeded |= requiresSteam && m_steamConfigManager
            && (m_startingSnapshot.syncSteamShortcuts || selectedShortcut);
    }
    if (steamRequired && m_steamConfigManager) m_steamConfigManager->refreshSourceAccounts();
    if (steamShortcutsNeeded && m_steamConfigManager) {
        if (!m_steamConfigManager->sourceAccountAvailable()) {
            Q_EMIT errorOccurred(m_steamConfigManager->sourceAccountError());
            setStatus(QStringLiteral("Error"));
            return false;
        }
        for (const InstanceConfig &instance : profile.instances) {
            if (instance.gameSelection.launcherId != QStringLiteral("steam")
                || instance.gameSelection.backend != QStringLiteral("shortcut")) continue;
            const auto games = m_steamConfigManager->gamesAsVariant();
            const bool found = std::any_of(games.cbegin(), games.cend(), [&](const QVariant &entry) {
                const QVariantMap game = entry.toMap();
                return game.value(QStringLiteral("gameId")).toString() == instance.gameSelection.gameId
                    && game.value(QStringLiteral("backend")).toString() == QStringLiteral("shortcut");
            });
            if (!found) {
                Q_EMIT errorOccurred(QStringLiteral("Selected Steam shortcut is unavailable in the source account"));
                setStatus(QStringLiteral("Error"));
                return false;
            }
        }
    }

    m_hasStartingProfile = true;

    setActive(true);
    setStatus(QStringLiteral("Starting session..."));
    if (!profile.preSessionExecutable.isEmpty()) {
        runHook(profile.preSessionExecutable, false);
        return true;
    }

    m_preHookCompleted = true;
    continueStart();
    return true;
}

void SessionRunner::continueStart()
{
    if (!m_active || m_finalizing) {
        return;
    }

    m_postHookArmed = true;
    inhibitScreenSaver();

    m_virtualOutputNames.clear();
    m_virtualRects.clear();
    startVirtualDisplays();
}

void SessionRunner::startVirtualDisplays()
{
    const SessionProfile &profile = activeProfile();
    QList<int> virtualIndices;
    for (int i = 0; i < profile.instances.size(); ++i) {
        if (profile.instances[i].display == InstanceConfig::DisplayVirtual) {
            virtualIndices.append(i);
        }
    }
    if (virtualIndices.isEmpty()) {
        continueStartAfterDisplays();
        return;
    }

    setStatus(QStringLiteral("Creating virtual displays..."));
    for (int idx : virtualIndices) {
        m_pendingDisplays.insert(idx);
    }
    for (int idx : virtualIndices) {
        if (m_finalizing) {
            return;
        }
        const QSize size = parseRenderResolution(profile.instances[idx].virtualResolution);
        m_virtualDisplays->create(idx, size.isValid() ? size : QSize(1920, 1080));
    }
}

void SessionRunner::continueStartAfterDisplays()
{
    if (!m_active || m_finalizing) {
        return;
    }
    setStatus(QStringLiteral("Starting session..."));

    const SessionProfile &profile = activeProfile();
    const int instanceCount = profile.instances.size();

    QList<int> mainIndices;
    QList<int> streamingIndices;
    for (int i = 0; i < instanceCount; ++i) {
        const InstanceConfig &instConfig = profile.instances[i];
        if (instConfig.display == InstanceConfig::DisplayMain) {
            mainIndices.append(i);
        } else if (instConfig.display == InstanceConfig::DisplayVirtual || instConfig.streamToMoonlight) {
            streamingIndices.append(i);
        }
    }

    m_streamingInstances.clear();
    if (!streamingIndices.isEmpty()) {
        // The helper restarts pipewire-pulse when it first opens cross-user audio access, which drops every
        // sink loaded before it. Do that now so the sinks created below survive until the game starts.
        if (!m_helperClient || !m_helperClient->setupRuntimeAccess()) {
            beginFinalization(true, QStringLiteral("Could not set up audio access for streaming"));
            return;
        }
        QString audioError;
        if (!m_streamAudio.prepare(streamingIndices, &audioError)) {
            beginFinalization(true, audioError);
            return;
        }
        for (int idx : streamingIndices) {
            StreamingInstanceInfo info;
            info.outputName = m_virtualOutputNames.value(idx,
                                                         InstanceConfig::monitorConnector(profile.instances[idx].display));
            info.sinkName = m_streamAudio.streamSink(idx);
            m_streamingInstances[idx] = info;
        }
    }

    // Players on the main screen split it; dedicated-monitor players fill their screen.
    m_layouts = QList<QRect>(instanceCount);
    const QList<QRect> mainLayouts = calculateLayout(profile.layout,
                                                     mainIndices.size(),
                                                     mainScreenGeometry(profile),
                                                     profile.gridSubLayout);
    for (int n = 0; n < mainIndices.size(); ++n) {
        m_layouts[mainIndices[n]] = mainLayouts.value(n);
    }
    for (int i = 0; i < instanceCount; ++i) {
        if (m_virtualRects.contains(i)) {
            m_layouts[i] = m_virtualRects.value(i);
        } else if (QScreen *screen = screenByConnector(InstanceConfig::monitorConnector(profile.instances[i].display))) {
            m_layouts[i] = screen->geometry();
        }
    }

    if (!setupDeviceOwnership()) {
        beginFinalization(true, QStringLiteral("Failed to set up device ownership"));
        return;
    }


    if (!setupSessionResources()) {
        beginFinalization(true, QStringLiteral("Failed to set up data directories"));
        return;
    }
    m_pendingInstanceConfigs.clear();
    for (int i = 0; i < instanceCount; ++i) {
        const InstanceConfig &instConfig = profile.instances[i];
        const bool isStreaming = m_streamingInstances.contains(i);
        QVariantMap config;
        config[QStringLiteral("username")] = instConfig.username;
        config[QStringLiteral("privateTemporaryDirectories")] = instConfig.privateTemporaryDirectories;
        {
            const QRect &rect = m_layouts[i];
            // Gamescope renders internally at -w/-h and scales to the window (-W/-H); an override lets a player
            // render below the window size, e.g. 720p in a 1080p region.
            const QSize render = parseRenderResolution(instConfig.renderResolution);
            config[QStringLiteral("internalWidth")] = render.isValid() ? render.width() : rect.width();
            config[QStringLiteral("internalHeight")] = render.isValid() ? render.height() : rect.height();
            config[QStringLiteral("outputWidth")] = rect.width();
            config[QStringLiteral("outputHeight")] = rect.height();
            config[QStringLiteral("positionX")] = rect.x();
            config[QStringLiteral("positionY")] = rect.y();
            const QString output = m_virtualOutputNames.value(
                i, InstanceConfig::monitorConnector(instConfig.display));
            if (!output.isEmpty()) {
                config[QStringLiteral("monitorName")] = output;
            }
            if (isStreaming) {
                config[QStringLiteral("sink")] = m_streamingInstances[i].sinkName;
                config[QStringLiteral("streamBitrate")] = instConfig.streamBitrate;
                config[QStringLiteral("streamCodec")] = instConfig.streamCodec;
                config[QStringLiteral("sunshinePort")] =
                    SunshineConfig::preferredPort(instConfig.username, i, instConfig.sunshinePort);
            } else if (!instConfig.audioDevice.isEmpty()) {
                // Players reach the host's PipeWire-Pulse over TCP, so a host sink name is valid for them.
                config[QStringLiteral("sink")] = instConfig.audioDevice;
            } else if (m_streamAudio.isActive() && !m_streamAudio.defaultSink().isEmpty()) {
                // Sunshine switches the host default sink while streaming; keep local players on the original.
                config[QStringLiteral("sink")] = m_streamAudio.defaultSink();
            }
        }
        config[QStringLiteral("refreshRate")] = instConfig.refreshRate;
        config[QStringLiteral("scalingMode")] = instConfig.scalingMode;
        config[QStringLiteral("filterMode")] = instConfig.filterMode;
        config[QStringLiteral("borderless")] = m_settingsManager ? m_settingsManager->borderlessWindows() : false;
        const QString presetId = instConfig.presetId.isEmpty() ? QStringLiteral("steam") : instConfig.presetId;
        config[QStringLiteral("presetId")] = presetId;
        config[QStringLiteral("launcherId")] = m_startingSnapshot.presets.at(i).launcherId;
        const LaunchCommand &command = m_startingSnapshot.launchCommands.at(i);
        config[QStringLiteral("gameCommand")] = QVariant::fromValue(
            QStringList{command.program} + command.arguments);
        config[QStringLiteral("workingDirectory")] = command.workingDirectory;

        if (m_deviceManager) {
            QVariantList pathList;
            for (const QString &path : m_deviceManager->getDevicePathsForInstance(i)) {
                pathList.append(path);
            }
            config[QStringLiteral("devicePaths")] = pathList;
        }
        // Directory preparations carry their own sources. Shared roots admit
        // only the selected command's working directory, not every data source.
        config[QStringLiteral("sharedRoots")] = command.workingDirectory.isEmpty()
            ? QStringList{} : QStringList{command.workingDirectory};
        m_pendingInstanceConfigs.append(config);
    }

    m_nextInstanceToStart = 0;
    startNextInstance();
}

void SessionRunner::beginFinalization(bool startupFailure, const QString &message)
{
    if (m_stoppingInstances) {
        return;
    }
    if (m_finalizing) {
        if (m_hookProcess) {
            m_hookProcess->kill();
            m_hookProcess->deleteLater();
            m_hookProcess = nullptr;
            finishFinalization();
            return;
        }
        {
            QScopedValueRollback<bool> stopping(m_stoppingInstances, true);
            if (m_streamManager) {
                m_streamManager->stopAll();
            }
            for (auto *instance : m_instances) {
                if (instance->pid() > 0) {
                    instance->stop();
                }
            }
        }
        continueFinalizationAfterStopping();
        return;
    }

    m_finalizing = true;
    m_finalizationCleanupStarted = false;
    m_startupFailure = startupFailure;
    m_finalizationMessage = message;

    if (m_hookProcess) {
        m_hookProcess->kill();
        m_hookProcess->deleteLater();
        m_hookProcess = nullptr;
        finishFinalization();
        return;
    }

    if (!m_postHookArmed && !m_sharedStateActive && m_instances.isEmpty()
        && m_streamingInstances.isEmpty() && (!m_streamManager || !m_streamManager->hasOwnedPids())) {
        finishFinalization();
        return;
    }

    setStatus(QStringLiteral("Stopping session..."));
    uninhibitScreenSaver();
    {
        // stopped() can be synchronous; retain resources until every stop returns.
        QScopedValueRollback<bool> stopping(m_stoppingInstances, true);
        if (m_streamManager) {
            m_streamManager->stopAll();
        }

        for (auto *instance : m_instances) {
            if (instance->pid() > 0) {
                instance->stop();
            }
        }
    }
    continueFinalizationAfterStopping();
}

bool SessionRunner::hasOwnedPids() const
{
    for (const auto *instance : m_instances) {
        if (instance->pid() > 0) {
            return true;
        }
    }
    return m_streamManager && m_streamManager->hasOwnedPids();
}

void SessionRunner::continueFinalizationAfterStopping()
{
    if (!m_finalizing || m_stoppingInstances || m_finalizationCleanupStarted || hasOwnedPids()) {
        return;
    }

    m_finalizationCleanupStarted = true;
    restoreDeviceOwnership();
    teardownSharingState();
    teardownStreamingInstances();
    cleanupInstances();

    const QString postHook = m_sessionManager ? activeProfile().postSessionExecutable : QString();
    if (m_postHookArmed && m_preHookCompleted && !postHook.isEmpty()) {
        m_postHookStarted = true;
        runHook(postHook, true);
        return;
    }
    finishFinalization();
}

void SessionRunner::finishFinalization()
{
    if (m_hookProcess) {
        m_hookProcess->deleteLater();
        m_hookProcess = nullptr;
    }
    const bool startupFailure = m_startupFailure;
    const QString message = m_finalizationMessage;
    setActive(false);
    setStatus(startupFailure ? QStringLiteral("Error") : QStringLiteral("Stopped"));
    Q_EMIT runningChanged();
    Q_EMIT instancesChanged();
    if (startupFailure) {
        if (!message.isEmpty()) {
            Q_EMIT errorOccurred(message);
        }
        Q_EMIT sessionStartFailed(message);
    } else {
        Q_EMIT sessionStopped();
    }
    m_sessionStartupComplete = false;
    m_finalizing = false;
    m_finalizationCleanupStarted = false;
    m_startupFailure = false;
    m_preHookCompleted = false;
    m_postHookArmed = false;
    m_postHookStarted = false;
    m_hookIsPost = false;
    m_finalizationMessage.clear();
    m_hasStartingProfile = false;
    m_startingSnapshot = {};
    resetGamepadAttribution();
}

void SessionRunner::stop()
{
    if (!m_active) {
        teardownSharingState();
        return;
    }
    beginFinalization(false);
}

void SessionRunner::stopInstance(int index)
{
    if (index >= 0 && index < m_instances.size()) {
        bool isStreaming = m_streamingInstances.contains(index);
        if (isStreaming) {
            m_streamManager->stopStream(index);
        }
        m_instances[index]->stop();
        if (isStreaming) {
            cleanupStreamingInstance(index);
        }
    }
}

bool SessionRunner::isRunning() const
{
    for (const auto *instance : m_instances) {
        if (instance->isRunning()) {
            return true;
        }
    }
    return false;
}

int SessionRunner::runningInstanceCount() const
{
    int count = 0;
    for (const auto *instance : m_instances) {
        if (instance->isRunning()) {
            ++count;
        }
    }
    return count;
}

QVariantList SessionRunner::instancesAsVariant() const
{
    QVariantList list;
    for (const auto *instance : m_instances) {
        QVariantMap map;
        map[QStringLiteral("index")] = instance->index();
        map[QStringLiteral("running")] = instance->isRunning();
        map[QStringLiteral("status")] = instance->status();
        map[QStringLiteral("pid")] = instance->pid();
        map[QStringLiteral("username")] = instance->username();

        QRect geom = instance->windowGeometry();
        map[QStringLiteral("x")] = geom.x();
        map[QStringLiteral("y")] = geom.y();
        map[QStringLiteral("width")] = geom.width();
        map[QStringLiteral("height")] = geom.height();

        list.append(map);
    }
    return list;
}

void SessionRunner::startNextInstance()
{
    if (m_nextInstanceToStart >= m_pendingInstanceConfigs.size()) {
        m_sessionStartupComplete = true;
        setStatus(QStringLiteral("Session running"));
        Q_EMIT runningChanged();
        Q_EMIT instancesChanged();
        Q_EMIT sessionStarted();
        return;
    }

    int index = m_nextInstanceToStart;
    const QVariantMap &config = m_pendingInstanceConfigs[index];

    auto *instance = new GamescopeInstance(this);
    instance->setHelperClient(m_helperClient);
    connect(instance, &GamescopeInstance::started, this, &SessionRunner::onInstanceStarted);
    connect(instance, &GamescopeInstance::stopped, this, &SessionRunner::onInstanceStopped);
    connect(instance, &GamescopeInstance::errorOccurred, this, &SessionRunner::onInstanceError);

    m_instances.append(instance);

    m_launchErrorReported = false;
    if (!instance->start(config, index, m_instanceDirectoryPreparations.value(index))) {
        qWarning() << "Failed to start instance" << index;
        if (!m_launchErrorReported) {
            beginFinalization(true, QStringLiteral("Failed to start instance %1").arg(index + 1));
        }
        return;
    }

}

void SessionRunner::cleanupInstances()
{
    if (m_windowManager) {
        m_windowManager->cancelAllRequests();
    }

    for (auto *instance : m_instances) {
        instance->deleteLater();
    }
    m_instances.clear();
    m_positionedWindowIds.clear();

    m_pendingInstanceConfigs.clear();
    m_layouts.clear();
    teardownStreamingInstances();
    m_streamingInstances.clear();
    m_nextInstanceToStart = 0;
    m_instanceDirectoryPreparations.clear();
}


bool SessionRunner::setupDeviceOwnership()
{
    if (!m_deviceManager || !m_helperClient) {
        return true;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, skipping device ownership setup";
        return true;
    }

    m_ownedDevicePaths.clear();

    if (!m_sessionManager) {
        return true;
    }

    const auto &profile = activeProfile();
    bool allSucceeded = true;

    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        QStringList devicePaths = m_deviceManager->getDevicePathsForInstance(i);

        if (username.isEmpty()) {
            for (const QString &path : devicePaths) {
                if (path.startsWith(QLatin1String("/dev/input/event"))) {
                    m_helperClient->watchDevice(path);
                    if (!m_ownedDevicePaths.contains(path)) {
                        m_ownedDevicePaths.append(path);
                    }
                }
            }
            continue;
        }


        const UserIdentity id = resolveUserIdentity(username, m_helperClient);
        if (!id.valid) {
            qWarning() << "SessionRunner: User" << username << "not found, skipping device ownership for instance" << i;
            continue;
        }
        int uid = static_cast<int>(id.uid);



        for (const QString &path : devicePaths) {
            if (m_helperClient->setDeviceOwner(path, uid)) {
                if (!m_ownedDevicePaths.contains(path)) {
                    m_ownedDevicePaths.append(path);
                }
            } else {
                qWarning() << "SessionRunner: Failed to set ownership of" << path;
                Q_EMIT errorOccurred(QStringLiteral("Failed to set device ownership for %1").arg(path));
                allSucceeded = false;
            }
        }

        QStringList hidrawPaths = m_deviceManager->getHidrawPathsForInstance(i);
        for (const QString &hidrawPath : hidrawPaths) {
            if (m_helperClient->setDeviceOwner(hidrawPath, uid)) {
                if (!m_ownedDevicePaths.contains(hidrawPath)) {
                    m_ownedDevicePaths.append(hidrawPath);
                }
                qDebug() << "SessionRunner: Set hidraw ownership" << hidrawPath << "for user" << username;

            } else {
                qWarning() << "SessionRunner: Failed to set hidraw ownership" << hidrawPath;
                allSucceeded = false;
            }
        }
    }

    return allSucceeded;

}

void SessionRunner::restoreDeviceOwnership()
{
    if (!m_helperClient || m_ownedDevicePaths.isEmpty()) {
        return;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, cannot restore device ownership";
        m_ownedDevicePaths.clear();
        return;
    }

    m_helperClient->restoreAllDevices();

    m_ownedDevicePaths.clear();
}

bool SessionRunner::setupSessionResources()
{
    if (!m_helperClient || !m_sessionManager || !m_presetManager) {
        return false;
    }



    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available for session resource setup";
        return false;
    }
    const bool shareSteamLibraries = m_startingSnapshot.shareSteamLibraries;
    // Arm the sharing-state tracker: teardown must run when the session ends,
    // including when the last game exits on its own
    m_sharedStateActive = true;
    m_steamSharedUsers.clear();
    m_instanceDirectoryPreparations.clear();
    // Both paths must come from the host; Flatpak may hide its home symlink.
    const QVariantMap homeInfo = m_helperClient->getUserHomeInfoByUid(static_cast<uint>(::getuid()));
    const QString compositorHome = homeInfo.value(QStringLiteral("home")).toString();
    const QString canonicalCompositorHome = homeInfo.value(QStringLiteral("canonicalHome")).toString();
    if (compositorHome.isEmpty() || canonicalCompositorHome.isEmpty()) {
        Q_EMIT errorOccurred(
            QStringLiteral("Could not determine host home paths; check the CouchPlay helper installation"));
        return false;
    }

    const auto &profile = activeProfile();
    bool allSucceeded = true;
    bool heroicCatalogLoaded = false;

    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        const QString presetId = profile.instances[i].presetId.isEmpty()
            ? QStringLiteral("steam") : profile.instances[i].presetId;
        if (username.isEmpty()) {
            continue;
        }

        const LaunchPreset preset = m_hasStartingProfile && i < m_startingSnapshot.presets.size()
            ? m_startingSnapshot.presets.at(i)
            : m_presetManager->getPreset(presetId.isEmpty() ? QStringLiteral("steam") : presetId);
        const QStringList requiredIntegrations = preset.requiredIntegrations;
        const bool selectedSteamShortcut = profile.instances[i].gameSelection.launcherId == QStringLiteral("steam")
            && profile.instances[i].gameSelection.backend == QStringLiteral("shortcut");
        const bool requiresSteam = requiredIntegrations.contains(QStringLiteral("steam")) || selectedSteamShortcut;
        const bool requiresHeroic = requiredIntegrations.contains(QStringLiteral("heroic"));


        // Steam shortcut sync: dispatched live at session start (not part of the
        // preset snapshot, so toggling the setting applies to the next session)


        if (requiresSteam && (!m_steamConfigManager || !m_steamConfigManager->isSteamDetected())) {
            qCWarning(couchplaySteam) << "Required integration is unavailable: steam";
            allSucceeded = false;
        }
        // The optional "sync all shortcuts" setting cannot apply to a player whose Steam has never run: Steam creates
        // the userdata tree on its first start, and the next session syncs normally. Only an explicitly selected
        // Steam shortcut game needs the sync, so only that case still fails the launch.
        const bool skipUnbootstrappedSync = requiresSteam && !selectedSteamShortcut && m_steamConfigManager
            && m_steamConfigManager->isSteamDetected() && m_startingSnapshot.syncSteamShortcuts
            && (!m_helperClient->isSteamBootstrapped(username)
                || m_steamConfigManager->getTargetSteamUserId(username).isEmpty());
        if (skipUnbootstrappedSync) {
            qCInfo(couchplaySteam) << "Skipping Steam shortcut sync for" << username
                                   << "- Steam has not been run for this user yet";
        }
        if (requiresSteam && m_steamConfigManager && m_steamConfigManager->isSteamDetected()
            && !skipUnbootstrappedSync && (m_startingSnapshot.syncSteamShortcuts || selectedSteamShortcut)) {
            if (!m_steamConfigManager->sourceAccountAvailable()) {
                Q_EMIT errorOccurred(m_steamConfigManager->sourceAccountError());
                return false;
            }
            qCDebug(couchplaySteam) << "Syncing Steam shortcuts for user" << username;
            m_steamConfigManager->loadShortcuts();
            const QStringList shortcutDirs = m_steamConfigManager->extractShortcutDirectories();
            for (const QString &dir : shortcutDirs) {
                if (QDir(dir).exists() && !m_helperClient->setPathAclWithParents(dir, username)) {
                    qCWarning(couchplaySteam) << "Failed to set ACL on shortcut directory" << dir;
                }
            }
            if (!m_steamConfigManager->syncShortcutsToUser(username)) {
                qCWarning(couchplaySteam) << "Failed to sync shortcuts to user" << username;
                allSucceeded = false;
            }
        }

        // Heroic: selective config sync (Flatpak-vs-native aware) plus optional
        // shortcut sync — replaces the generic whole-directory copy of the
        // config root, mirroring the pre-refactor setupLauncherAccess behavior
        if (requiresHeroic && (!m_heroicConfigManager || !m_heroicConfigManager->isHeroicDetected())) {
            qCWarning(couchplaySteam) << "Required integration is unavailable: heroic";
            allSucceeded = false;
        }
        if (requiresHeroic && m_heroicConfigManager && m_heroicConfigManager->isHeroicDetected()) {
            if (!heroicCatalogLoaded) {
                // Refresh once per session; snapshots may predate installed games.
                m_heroicConfigManager->loadGames();
                heroicCatalogLoaded = true;
            }
            qCDebug(couchplaySteam) << "Syncing Heroic config for user" << username;
            if (!m_heroicConfigManager->syncConfigToUser(username)) {
                qCWarning(couchplaySteam) << "Failed to sync Heroic config to" << username;
                allSucceeded = false;
            }
            if (m_startingSnapshot.syncHeroicShortcuts) {
                qCDebug(couchplaySteam) << "Syncing Heroic shortcuts for user" << username;
                if (!m_heroicConfigManager->syncShortcutsToUser(username)) {
                    qCWarning(couchplaySteam) << "Failed to sync Heroic shortcuts to" << username;
                    allSucceeded = false;
                }
            }
        }

        const QList<DataDirectory> dataDirs = preset.dataDirectories;
        QVariantList directoryPlans;
        QSet<QString> plannedAclPaths;
        const auto isCoveredByConfiguredAcl = [&dataDirs](const QString &path) {
            for (const DataDirectory &dir : dataDirs) {
                if (dir.mode != QStringLiteral("acl") || !QDir::isAbsolutePath(dir.path)) {
                    continue;
                }
                const QString aclPath = QDir::cleanPath(dir.path);
                if (path == aclPath || aclPath == QStringLiteral("/")
                    || path.startsWith(aclPath + QLatin1Char('/'))) {
                    return true;
                }
            }
            return false;
        };
        const auto appendAclPreparation = [&directoryPlans, &plannedAclPaths](const QString &path) {
            const QString cleanPath = QDir::cleanPath(path);
            if (plannedAclPaths.contains(cleanPath)) {
                return;
            }
            plannedAclPaths.insert(cleanPath);
            directoryPlans.append(DirectoryPreparation{path, QStringLiteral("acl"), {}, {}}.toVariant());
        };
        if (i < m_startingSnapshot.launchCommands.size()) {
            const QStringList applicationDirectories =
                applicationAccessDirectories(m_startingSnapshot.launchCommands.at(i), compositorHome);
            for (const QString &directory : applicationDirectories) {
                const QString applicationDirectory = QDir::cleanPath(directory);
                if (isCoveredByConfiguredAcl(applicationDirectory)) {
                    continue;
                }

                if (!m_helperClient->setPathAclWithParents(applicationDirectory, username)) {
                    qCWarning(couchplaySteam) << "Failed to set ACL for selected application directory"
                                              << applicationDirectory << "user" << username;
                    allSucceeded = false;
                }
                appendAclPreparation(applicationDirectory);
            }
        }

        QSet<QString> gameAccessDirectories;
        if (requiresHeroic && m_heroicConfigManager && m_heroicConfigManager->isHeroicDetected()) {
            auto addGameAccessDirectory = [&gameAccessDirectories, &compositorHome](const QString &path) {
                if (path.isEmpty() || !QDir::isAbsolutePath(path) || !QDir(path).exists()) {
                    return;
                }
                LaunchCommand accessCheck;
                accessCheck.workingDirectory = path;
                const QStringList safeDirectories = applicationAccessDirectories(accessCheck, compositorHome);
                if (!safeDirectories.isEmpty()) {
                    gameAccessDirectories.insert(safeDirectories.constFirst());
                }
            };

            for (const HeroicGame &game : m_heroicConfigManager->installedGames()) {
                addGameAccessDirectory(game.installPath);

                QString executablePath = game.executable;
                if (executablePath.isEmpty()) {
                    continue;
                }
                if (!QDir::isAbsolutePath(executablePath)) {
                    if (!QDir::isAbsolutePath(game.installPath)) {
                        continue;
                    }
                    executablePath = QDir(game.installPath).filePath(executablePath);
                }
                addGameAccessDirectory(QFileInfo(executablePath).absolutePath());
            }

            for (const QString &path : gameAccessDirectories) {
                bool coveredByAclDataDirectory = false;
                for (const DataDirectory &dir : dataDirs) {
                    if (dir.mode != QStringLiteral("acl")) {
                        continue;
                    }
                    const QString aclPath = QDir::cleanPath(dir.path);
                    const bool coveredByAcl = path == aclPath
                        || (path.startsWith(aclPath + QLatin1Char('/')) && !gameAccessDirectories.contains(aclPath));
                    if (coveredByAcl) {
                        coveredByAclDataDirectory = true;
                        break;
                    }
                }
                if (coveredByAclDataDirectory) {
                    continue;
                }

                // Grant traversal to registered game paths; selected data roots are ACLed below.
                if (!m_helperClient->setPathAclWithParents(path, username)) {
                    qCWarning(couchplaySteam)
                        << "Failed to set traversal ACL for Heroic game path" << path << "user" << username;
                    allSucceeded = false;
                }
            }
        }
        if (dataDirs.isEmpty()) {
            m_instanceDirectoryPreparations[i] = directoryPlans;
            qDebug() << "SessionRunner: No data directories for instance" << i << "user" << username;
            continue;
        }

        qDebug() << "SessionRunner: Setting up" << dataDirs.size() << "data directories for user" << username;

        for (const DataDirectory &dir : dataDirs) {
            // Library sharing is opt-in: the steamRoot overlay entry is a
            // marker handled entirely by prepareDataDir (libraries are
            // alias-mounted under ~/.couchplay/steam-libs/<i> so the player's
            // own Steam root, account state and userdata stay untouched) —
            // never mount it at the home-relative path, which would shadow
            // the player's installation
            // The Steam-root overlay marker represents library sharing, not a generic directory.
            // Never expose the compositor account root for a non-Steam launcher.
            if (dir.mode == QStringLiteral("overlay") && m_steamConfigManager
                && m_steamConfigManager->isSteamRootPath(dir.path)) {
                if (!requiresSteam) {
                    qCWarning(couchplaySteam) << "Ignoring Steam root data marker for non-Steam launcher" << username;
                    continue;
                }
                if (!shareSteamLibraries) {
                    qDebug() << "SessionRunner: Library sharing disabled, skipping Steam root entry for" << dir.path;
                    continue;
                }
                // Finalization writes manifests and libraryfolders.vdf
                // entries for the alias mounts — skip it when preparation
                if (!m_steamConfigManager->isSteamDetected()) {
                    Q_EMIT errorOccurred(QStringLiteral("Selected Steam installation is unavailable"));
                    allSucceeded = false;
                    continue;
                }
                DataDirectory steamMarker = dir;
                steamMarker.path = m_steamConfigManager->steamPaths().steamRoot;
                if (!m_steamConfigManager->prepareDataDir(steamMarker, username, directoryPlans)) {
                    qCWarning(couchplaySteam) << "Steam library sharing failed for" << dir.path;
                    allSucceeded = false;
                    continue;
                }
                m_steamSharedUsers.insert(username);
                if (!m_steamConfigManager->finalizeDataDir(steamMarker, username)) {
                    qCWarning(couchplaySteam) << "Steam library finalize failed for" << dir.path;
                    allSucceeded = false;
                }
                continue;
            }

            // Heroic config is synchronized selectively above; skip a generic copy of its root.
            if (dir.mode == QStringLiteral("copy") && requiresHeroic && m_heroicConfigManager
                && dir.path == m_heroicConfigManager->configPath()) {
                qDebug() << "SessionRunner: Heroic config handled by config sync, skipping copy of" << dir.path;
                continue;
            }

            const HomePathMapping mapping = mapHomeRelativePath(dir.path, compositorHome, canonicalCompositorHome);
            if (mapping.kind == HomePathKind::Invalid) {
                Q_EMIT errorOccurred(QStringLiteral("Invalid shared directory: %1").arg(dir.path));
                allSucceeded = false;
                continue;
            }
            QString targetAlias;
            if (dir.mode == QStringLiteral("copy") && mapping.kind == HomePathKind::External) {
                targetAlias = QStringLiteral(".couchplay/copies/")
                    + DirectoryPlan::dataDirectoryStagingSlug(dir.path, compositorHome);
            } else if (mapping.kind == HomePathKind::Relative) {
                targetAlias = mapping.relativePath;
            } else {
                targetAlias = QStringLiteral(".couchplay/mounts") + QDir::cleanPath(dir.path);
            }

            if (dir.mode == QStringLiteral("acl")) {
                if (!plannedAclPaths.contains(QDir::cleanPath(dir.path))) {
                    const bool parentsOk = m_helperClient->setPathAclWithParents(dir.path, username);
                    appendAclPreparation(dir.path);
                    if (!parentsOk) {
                        qWarning() << "SessionRunner: Failed to set ACL for" << dir.path << "user" << username;
                        allSucceeded = false;
                    }
                }
                continue;
            }

            QString stagingPath;
            if (dir.mode == QStringLiteral("copy") || dir.mode == QStringLiteral("overlay")) {
                const QString stagingDir = playerDataStagingRoot(presetId, username) + QLatin1Char('/')
                    + DirectoryPlan::dataDirectoryStagingSlug(dir.path, compositorHome);
                if (QDir(stagingDir).exists()) {
                    // The GUI may use a caller-owned storage alias. Pass the
                    // resolved directory; the helper still walks it no-follow.
                    stagingPath = QFileInfo(stagingDir).canonicalFilePath();
                    if (stagingPath.isEmpty()) {
                        Q_EMIT errorOccurred(
                            QStringLiteral("Could not resolve player staging directory: %1").arg(stagingDir));
                        allSucceeded = false;
                        continue;
                    }
                }
            }
            directoryPlans.append(DirectoryPreparation{dir.path, dir.mode, targetAlias, stagingPath}.toVariant());
            if (requiresSteam && m_steamConfigManager
                && !m_steamConfigManager->finalizeDataDir(dir, username)) {
                qCWarning(couchplaySteam) << "Steam finalizeDataDir failed for" << dir.path;
                allSucceeded = false;
            }
        }
        m_instanceDirectoryPreparations[i] = directoryPlans;

    }


    return allSucceeded;
}


void SessionRunner::teardownSharingState()
{
    // Restore generated per-player Steam configuration. Directory views are
    // owned by each instance unit and disappear with its control group.
    if (!m_sharedStateActive) {
        return;
    }
    m_sharedStateActive = false;


    if (m_steamConfigManager && !m_steamSharedUsers.isEmpty()) {
        const QSet<QString> sharedUsers = m_steamSharedUsers;
        m_steamSharedUsers.clear();
        for (const QString &username : sharedUsers) {
            if (!m_steamConfigManager->cleanupLibrarySharing(username)) {
                qCWarning(couchplaySteam) << "Failed to clean up Steam sharing for" << username;
            }
        }
    }
}

QScreen *SessionRunner::screenByConnector(const QString &connector)
{
    if (connector.isEmpty()) {
        return nullptr;
    }
    const auto screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (screen->name() == connector) {
            return screen;
        }
    }
    return nullptr;
}

QRect SessionRunner::mainScreenGeometry(const SessionProfile &profile)
{
    QScreen *screen = screenByConnector(profile.mainScreen);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen) {
        return screen->geometry();
    }

    return QRect(0, 0, 1920, 1080);
}

QString SessionRunner::validateDisplays(const SessionProfile &profile)
{
    QStringList connectors;
    for (QScreen *screen : QGuiApplication::screens()) {
        connectors.append(screen->name());
    }
    QScreen *primary = QGuiApplication::primaryScreen();
    return validateDisplays(profile, connectors, primary ? primary->name() : QString());
}

QString SessionRunner::validateDisplays(const SessionProfile &profile,
                                        const QStringList &connectors,
                                        const QString &primaryConnector)
{
    const QString mainName = connectors.contains(profile.mainScreen) && !profile.mainScreen.isEmpty()
        ? profile.mainScreen
        : primaryConnector;

    bool anyMainPlayer = false;
    QMap<QString, int> monitorOwner;
    for (int i = 0; i < profile.instances.size(); ++i) {
        const InstanceConfig &instance = profile.instances[i];
        if (instance.display == InstanceConfig::DisplayMain) {
            anyMainPlayer = true;
            if (instance.streamToMoonlight) {
                return QStringLiteral("Player %1: streaming needs a dedicated monitor or a virtual display").arg(i + 1);
            }
            continue;
        }
        const QString connector = InstanceConfig::monitorConnector(instance.display);
        if (connector.isEmpty()) {
            continue; // virtual display
        }
        if (!connectors.contains(connector)) {
            return QStringLiteral("Player %1: monitor %2 is not connected").arg(i + 1).arg(connector);
        }
        if (monitorOwner.contains(connector)) {
            return QStringLiteral("Players %1 and %2 cannot share monitor %3")
                .arg(monitorOwner.value(connector) + 1)
                .arg(i + 1)
                .arg(connector);
        }
        monitorOwner.insert(connector, i);
    }
    if (anyMainPlayer && monitorOwner.contains(mainName)) {
        return QStringLiteral("Monitor %1 is the main screen; choose another main screen or move the players")
            .arg(mainName);
    }
    return {};
}

QString SessionRunner::validatePairing(const SessionProfile &profile)
{
    return validatePairing(profile, [](const QString &username) { return SunshineConfig::pairedClientCount(username); });
}

QString SessionRunner::validatePairing(const SessionProfile &profile,
                                       const std::function<int(const QString &username)> &pairedCount)
{
    for (int i = 0; i < profile.instances.size(); ++i) {
        const InstanceConfig &instance = profile.instances[i];
        const bool streaming = instance.display != InstanceConfig::DisplayMain
            && (instance.display == InstanceConfig::DisplayVirtual || instance.streamToMoonlight);
        if (!streaming || instance.username.isEmpty()) {
            continue;
        }
        if (pairedCount(instance.username) == 0) {
            return QStringLiteral("Player %1 (%2) has no paired Moonlight client. "
                                  "Pair one for %2 on the Users page first.")
                .arg(i + 1)
                .arg(instance.username);
        }
    }
    return {};
}

QSize SessionRunner::parseRenderResolution(const QString &text)
{
    const QStringList parts = text.trimmed().split(QLatin1Char('x'));
    if (parts.size() != 2) {
        return {};
    }
    bool widthOk = false;
    bool heightOk = false;
    const int width = parts.at(0).toInt(&widthOk);
    const int height = parts.at(1).toInt(&heightOk);
    if (!widthOk || !heightOk || width < 320 || width > 7680 || height < 200 || height > 4320) {
        return {};
    }
    return {width, height};
}

QList<QRect> SessionRunner::calculateLayout(const QString &layout,
                                            int instanceCount,
                                            const QRect &screenGeometry,
                                            const QString &gridSubLayout)
{
    QList<QRect> result;

    if (instanceCount < 1) {
        return result;
    }

    int x = screenGeometry.x();
    int y = screenGeometry.y();
    int w = screenGeometry.width();
    int h = screenGeometry.height();

    if (layout == QStringLiteral("horizontal")) {
        // Side by side (equal width)
        int instanceWidth = w / instanceCount;
        for (int i = 0; i < instanceCount; ++i) {
            result.append(QRect(x + i * instanceWidth, y, instanceWidth, h));
        }
    } else if (layout == QStringLiteral("vertical")) {
        // Stacked top to bottom (equal height)
        int instanceHeight = h / instanceCount;
        for (int i = 0; i < instanceCount; ++i) {
            result.append(QRect(x, y + i * instanceHeight, w, instanceHeight));
        }
    } else if (layout == QStringLiteral("grid")) {
        // 3-player grid sub-layouts:
        //   "horizontal" (default): 3x1
        //   "grid-2x2": 2x2 with empty cell
        //   "left-right": player 1 left 40%, players 2+3 stacked right 60%
        if (instanceCount == 3 && gridSubLayout == QStringLiteral("left-right")) {
            int leftWidth = w * 2 / 5;
            int rightWidth = w - leftWidth;
            int halfHeight = h / 2;
            result.append(QRect(x, y, leftWidth, h));
            result.append(QRect(x + leftWidth, y, rightWidth, halfHeight));
            result.append(QRect(x + leftWidth, y + halfHeight, rightWidth, h - halfHeight));
        } else {
            int cols, rows;
            if (instanceCount == 3) {
                if (gridSubLayout == QStringLiteral("grid-2x2")) {
                    cols = 2;
                    rows = 2;
                } else {
                    // Default for 3 players: horizontal (3×1)
                    cols = 3;
                    rows = 1;
                }
            } else if (instanceCount <= 2) {
                cols = 2;
                rows = 1;
            } else {
                cols = 2;
                rows = 2;
            }

            int cellWidth = w / cols;
            int cellHeight = h / rows;

            for (int i = 0; i < instanceCount; ++i) {
                int col = i % cols;
                int row = i / cols;
                result.append(QRect(x + col * cellWidth, y + row * cellHeight, cellWidth, cellHeight));
            }
        }
    } else {
        int instanceWidth = w / instanceCount;
        for (int i = 0; i < instanceCount; ++i) {
            result.append(QRect(x + i * instanceWidth, y, instanceWidth, h));
        }
    }

    return result;
}

void SessionRunner::onInstanceStarted()
{
    auto *instance = qobject_cast<GamescopeInstance *>(sender());
    if (!instance || m_finalizing) {
        return;
    }

    const int idx = instance->index();
    if (idx != m_nextInstanceToStart) {
        return;
    }

    // The helper's first launch restarts pipewire-pulse, which unloads the stream sinks
    if (!m_streamAudio.ensureSinks()) {
        beginFinalization(true, QStringLiteral("Could not restore the stream audio sinks"));
        return;
    }

    Q_EMIT instanceStarted(idx);
    Q_EMIT instancesChanged();
    Q_EMIT runningInstanceCountChanged();

    if (m_streamingInstances.contains(idx)) {
        m_streamStartupErrorReported = false;
        const InstanceConfig &instConfig = activeProfile().instances.value(idx);
        const QVariantMap &config = m_pendingInstanceConfigs.value(idx);
        StreamSpec spec;
        spec.instanceIndex = idx;
        spec.identityKey = instConfig.username;
        spec.outputName = m_streamingInstances[idx].outputName;
        spec.sinkName = m_streamingInstances[idx].sinkName;
        spec.port = SunshineConfig::allocatePort(config.value(QStringLiteral("sunshinePort"),
                                                              SunshineConfig::BASE_PORT).toInt());
        spec.bitrate = instConfig.streamBitrate;
        spec.codec = instConfig.streamCodec;
        spec.displayName = QStringLiteral("CouchPlay – Player %1 (%2)").arg(idx + 1).arg(instConfig.username);
        if (spec.port < 0 || !m_streamManager) {
            beginFinalization(true, QStringLiteral("Player %1: no free Sunshine ports").arg(idx + 1));
            return;
        }
        if (m_settingsManager) {
            m_streamManager->setSunshinePath(m_settingsManager->resolvedSunshinePath());
        }
        // Continue launching once Sunshine reports ready (see StreamManager::streamStarted).
        if (!m_streamManager->startStream(idx, spec) && !m_finalizing && !m_streamStartupErrorReported) {
            beginFinalization(true, QStringLiteral("Failed to start streaming instance %1").arg(idx + 1));
        }
        return;
    }
    proceedAfterInstanceStarted(instance);
}

void SessionRunner::proceedAfterInstanceStarted(GamescopeInstance *instance)
{
    if (m_windowManager && m_windowManager->isAvailable()) {
        positionInstanceWindow(instance);
    } else {
        ++m_nextInstanceToStart;
        startNextInstance();
    }
}

void SessionRunner::onInstanceStopped()
{
    if (!m_active && !m_hasStartingProfile) {
        return;
    }
    auto *instance = qobject_cast<GamescopeInstance *>(sender());
    if (!instance) {
        return;
    }

    const int idx = instance->index();
    if (m_windowManager) {
        // The replacement-window listener is PID-scoped; it must not outlive its Gamescope process.
        m_windowManager->cancelPositionRequest(idx);
    }
    if (m_streamingInstances.contains(idx)) {
        bool stopRequested = false;
        if (m_streamManager) {
            stopRequested = m_streamManager->stopStream(idx);
        }
        if (stopRequested || !m_streamManager || !m_streamManager->hasOwnedPid(idx)) {
            cleanupStreamingInstance(idx);
        }
    }

    Q_EMIT instanceStopped(idx);
    Q_EMIT instancesChanged();
    Q_EMIT runningInstanceCountChanged();

    if (m_finalizing) {
        continueFinalizationAfterStopping();
        return;
    }
    if (!m_sessionStartupComplete) {
        beginFinalization(true,
                          QStringLiteral("Instance %1 stopped before session startup completed").arg(idx + 1));
        return;
    }
    bool anyOwnedPid = false;
    for (GamescopeInstance *activeInstance : m_instances) {
        if (activeInstance->pid() > 0) {
            anyOwnedPid = true;
            break;
        }
    }
    if (!anyOwnedPid) {
        beginFinalization(false);
    }
}
void SessionRunner::onInstanceError(const QString &message)
{
    if (!m_active && !m_hasStartingProfile) {
        return;
    }
    if (m_sessionStartupComplete && message == QStringLiteral("exited")) {
        return;
    }
    auto *instance = qobject_cast<GamescopeInstance *>(sender());
    QString fullMessage;
    if (instance) {
        fullMessage = QStringLiteral("Instance %1: %2").arg(instance->index()).arg(message);
    } else {
        fullMessage = message;
    }
    Q_EMIT errorOccurred(fullMessage);
    if (!m_sessionStartupComplete && !m_finalizing) {
        if (instance && instance->index() == m_nextInstanceToStart) {
            m_launchErrorReported = true;
        }
        beginFinalization(true, fullMessage);
    }
}

void SessionRunner::positionInstanceWindow(GamescopeInstance *instance)
{
    if (!instance || !m_windowManager || !m_windowManager->isAvailable()) {
        return;
    }

    QRect targetGeometry = instance->windowGeometry();
    int instanceIndex = instance->index();

    const bool borderless = m_settingsManager && m_settingsManager->borderlessWindows();

    // The gamescope window only appears once the game client draws. A gaming user's first Steam launch
    // downloads and applies a client update first, which routinely takes longer than a minute.
    constexpr int windowAppearTimeoutMs = 5 * 60 * 1000;
    m_windowManager->queuePositionRequest(instanceIndex, instance->gamescopePid(), targetGeometry,
                                         m_positionedWindowIds, borderless, windowAppearTimeoutMs);
}

void SessionRunner::onWindowPositioned(int requestId, const QString &windowId)
{
    if (m_finalizing || requestId != m_nextInstanceToStart) {
        return;
    }
    GamescopeInstance *positionedInstance = nullptr;
    for (GamescopeInstance *instance : m_instances) {
        if (instance->index() == requestId) {
            positionedInstance = instance;
            break;
        }
    }
    if (!positionedInstance || !positionedInstance->isRunning()) {
        return;
    }

    if (!m_positionedWindowIds.contains(windowId)) {
        m_positionedWindowIds.append(windowId);
    }
    ++m_nextInstanceToStart;
    startNextInstance();
}

void SessionRunner::onWindowPositioningTimeout(int requestId)
{
    qWarning() << "SessionRunner: Failed to position window for instance" << requestId
               << "after timeout - stopping session";
    beginFinalization(true, QStringLiteral("Failed to position window for instance %1. Session stopped.").arg(requestId));
}

void SessionRunner::setupGlobalShortcut()
{
    m_stopAction = new QAction(this);
    m_stopAction->setObjectName(QStringLiteral("stop-couchplay-session"));
    m_stopAction->setText(i18nc("@action", "Stop CouchPlay Session"));
    m_stopAction->setProperty("componentName", QStringLiteral("couchplay"));

    connect(m_stopAction, &QAction::triggered, this, [this]() {
        if (isActive()) {
            stop();
        }
    });

    // Default shortcut: Meta+Shift+Escape
    KGlobalAccel::setGlobalShortcut(m_stopAction,
                                    QList<QKeySequence>() << QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_Escape));
}

void SessionRunner::onDeviceReconnected(const QString &stableId, int eventNumber, int instanceIndex)
{
    if (!m_active || !m_hasStartingProfile || m_finalizing || !m_preHookCompleted) {
        return;
    }

    if (!m_helperClient || !m_sessionManager) {
        return;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, cannot restore device ownership";
        return;
    }

    const auto &profile = activeProfile();
    if (instanceIndex < 0 || instanceIndex >= profile.instances.size()) {
        qWarning() << "SessionRunner: Invalid instance index" << instanceIndex << "for reconnected device";
        return;
    }

    qDebug() << "SessionRunner: Device reconnected, restoring ownership (stableId:" << stableId << ")";
    grantDeviceToInstance(eventNumber, instanceIndex);
}

qint64 SessionRunner::nowMs() const
{
    return m_clockOverrideMs >= 0 ? m_clockOverrideMs : m_clock.elapsed();
}

void SessionRunner::resetGamepadAttribution()
{
    m_gamepadClaims.clear();
    m_sunshineAttributions.clear();
}

bool SessionRunner::grantDeviceToInstance(int eventNumber, int instanceIndex)
{
    if (!m_helperClient || !m_sessionManager || !m_hasStartingProfile) {
        return false;
    }
    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, cannot set device ownership";
        return false;
    }

    const auto &profile = activeProfile();
    if (instanceIndex < 0 || instanceIndex >= profile.instances.size()) {
        qWarning() << "SessionRunner: Invalid instance index" << instanceIndex << "for device" << eventNumber;
        return false;
    }

    const QString &username = profile.instances[instanceIndex].username;
    QString devicePath = QStringLiteral("/dev/input/event%1").arg(eventNumber);

    if (username.isEmpty()) {
        qDebug() << "SessionRunner: Device in host session, watching:" << devicePath;
        if (m_helperClient->watchDevice(devicePath)) {
            if (!m_ownedDevicePaths.contains(devicePath)) {
                m_ownedDevicePaths.append(devicePath);
            }
            return true;
        }
        return false;
    }

    const UserIdentity id = resolveUserIdentity(username, m_helperClient);
    if (!id.valid) {
        qWarning() << "SessionRunner: User" << username << "not found";
        return false;
    }
    int uid = static_cast<int>(id.uid);

    bool granted = false;
    // Prevent infinite loop: if the device node is already owned by the target user, udev has already
    // applied the custom rule and we do not need to trigger another unbind/rebind.
    QFileInfo fileInfo(devicePath);
    if (fileInfo.exists() && fileInfo.ownerId() == id.uid) {
        qDebug() << "SessionRunner: Device" << devicePath << "is already owned by" << username << "(UID:" << uid << "), skipping ownership update";
        if (devicePath.startsWith(QLatin1String("/dev/input/event"))) {
            m_helperClient->watchDevice(devicePath);
        }
        return true;
    }

    qDebug() << "SessionRunner: Setting ownership of" << devicePath << "to user" << username;
    if (m_helperClient->setDeviceOwner(devicePath, uid)) {
        granted = true;
        if (!m_ownedDevicePaths.contains(devicePath)) {
            m_ownedDevicePaths.append(devicePath);
        }
        qDebug() << "SessionRunner: Successfully set ownership of" << devicePath;
    } else {
        qWarning() << "SessionRunner: Failed to set ownership of" << devicePath;
        Q_EMIT errorOccurred(QStringLiteral("Failed to restore device ownership for %1").arg(devicePath));
    }

    QString hidrawPath = m_deviceManager ? m_deviceManager->findHidrawForEvent(eventNumber) : QString();
    if (!hidrawPath.isEmpty()) {
        QFileInfo hidrawInfo(hidrawPath);
        if (hidrawInfo.exists() && hidrawInfo.ownerId() != id.uid) {
            if (m_helperClient->setDeviceOwner(hidrawPath, uid)) {
                if (!m_ownedDevicePaths.contains(hidrawPath)) {
                    m_ownedDevicePaths.append(hidrawPath);
                }
                qDebug() << "SessionRunner: Set hidraw ownership:" << hidrawPath;
            }
        } else {
            qDebug() << "SessionRunner: Hidraw" << hidrawPath << "is already owned by" << username << ", skipping ownership update";
        }
    }
    return granted;
}

void SessionRunner::onDeviceAdded(int eventNumber, const QString &name)
{
    if (!m_active || !m_hasStartingProfile || m_finalizing || !m_preHookCompleted) {
        return;
    }
    if (!m_deviceManager || !name.startsWith(QLatin1String("Sunshine "))) {
        return;
    }

    const QVariantMap device = m_deviceManager->getDevice(eventNumber);
    if (device.isEmpty()) {
        return;
    }
    // Remote keyboard/mouse act on the desktop seat and are left to KWin.
    const QString type = device.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("keyboard") || type == QLatin1String("mouse")) {
        return;
    }

    const QString uniq = device.value(QStringLiteral("uniq")).toString();
    const qint64 now = nowMs();
    m_sunshineAttributions.removeIf([&](const SunshineAttribution &a) {
        return now - a.atMs > m_uniqWindowMs;
    });
    m_gamepadClaims.removeIf([&](const GamepadClaim &c) {
        return now - c.atMs > m_claimTtlMs;
    });

    int target = -1;
    if (!uniq.isEmpty()) {
        for (const SunshineAttribution &a : std::as_const(m_sunshineAttributions)) {
            if (a.uniq == uniq) {
                target = a.instanceIndex;
                break;
            }
        }
    }
    if (target < 0 && !m_gamepadClaims.isEmpty()) {
        target = m_gamepadClaims.takeFirst().instanceIndex;
    }
    if (target < 0 && m_streamingInstances.size() == 1) {
        target = m_streamingInstances.firstKey();
    }
    if (target < 0) {
        qCWarning(couchplayCore) << "Cannot attribute Moonlight gamepad" << name << "(event" << eventNumber
                                 << ") to a streaming player; leaving it unowned";
        return;
    }

    if (grantDeviceToInstance(eventNumber, target) && !uniq.isEmpty()) {
        m_sunshineAttributions.append({uniq, target, now});
    }
}

QList<qint64> SessionRunner::getGamescopePids() const
{
    QList<qint64> pids;
    for (const auto *instance : m_instances) {
        if (instance && instance->gamescopePid() > 0) {
            pids.append(instance->gamescopePid());
        }
    }
    return pids;
}


void SessionRunner::inhibitScreenSaver()
{
    if (m_screenSaverCookie != 0) {
        return;
    }

    QDBusInterface screenSaver(QStringLiteral("org.freedesktop.ScreenSaver"),
                               QStringLiteral("/org/freedesktop/ScreenSaver"),
                               QStringLiteral("org.freedesktop.ScreenSaver"),
                               QDBusConnection::sessionBus());

    if (!screenSaver.isValid()) {
        qCDebug(couchplayCore) << "ScreenSaver D-Bus interface not available, skipping inhibition";
        return;
    }

    QDBusReply<uint> reply = screenSaver.call(QStringLiteral("Inhibit"),
                                              QStringLiteral("io.github.hikaps.couchplay"),
                                              QStringLiteral("Split-screen gaming session active"));

    if (reply.isValid()) {
        m_screenSaverCookie = reply.value();
        qCDebug(couchplayCore) << "ScreenSaver inhibited, cookie:" << m_screenSaverCookie;
    } else {
        qCWarning(couchplayCore) << "Failed to inhibit ScreenSaver:" << reply.error().message();
    }
}

void SessionRunner::uninhibitScreenSaver()
{
    if (m_screenSaverCookie == 0) {
        return;
    }

    QDBusInterface screenSaver(QStringLiteral("org.freedesktop.ScreenSaver"),
                               QStringLiteral("/org/freedesktop/ScreenSaver"),
                               QStringLiteral("org.freedesktop.ScreenSaver"),
                               QDBusConnection::sessionBus());

    if (screenSaver.isValid()) {
        screenSaver.call(QStringLiteral("UnInhibit"), m_screenSaverCookie);
        qCDebug(couchplayCore) << "ScreenSaver uninhibited, cookie:" << m_screenSaverCookie;
    }

    m_screenSaverCookie = 0;
}

void SessionRunner::teardownStreamingInstances()
{
    m_streamingInstances.clear();
    m_streamAudio.teardown();
    m_pendingDisplays.clear();
    if (m_virtualDisplays) {
        m_virtualDisplays->destroyAll();
    }
}

void SessionRunner::cleanupStreamingInstance(int index)
{
    if (!m_streamingInstances.contains(index)
        || (m_streamManager && m_streamManager->hasOwnedPid(index))) {
        return;
    }
    for (const auto *instance : m_instances) {
        if (instance->index() == index && instance->pid() > 0) {
            return;
        }
    }

    m_streamingInstances.remove(index);
}
