// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SessionRunner.h"
#include "../../helper/HomePathMapping.h"
#include "../../helper/DirectoryPlan.h"
#include "../dbus/CouchPlayHelperClient.h"
#include "DeviceManager.h"
#include "GamescopeInstance.h"
#include "Logging.h"
#include "PresetManager.h"
#include "SessionManager.h"
#include "SettingsManager.h"
#include "SteamConfigManager.h"
#include "StreamManager.h"
#include "UserLookup.h"
#include "WindowManager.h"

#include <QAction>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QMetaObject>
#include <QScreen>
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

    m_streamManager = new StreamManager(this);
    m_streamManager->setHelperClient(m_helperClient);
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
        ++m_nextInstanceToStart;
        startNextInstance();
    });
    connect(m_streamManager, &StreamManager::streamStopped, this, [this](int instanceIndex) {
        cleanupStreamingInstance(instanceIndex);
        QMetaObject::invokeMethod(this, [this]() {
            if (m_finalizing) {
                continueFinalizationAfterStopping();
            }
        }, Qt::QueuedConnection);
    });
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
const SessionProfile &SessionRunner::activeProfile() const
{
    return m_hasStartingProfile ? m_startingSnapshot.profile : m_sessionManager->currentProfile();
}


void SessionRunner::runHook(const QString &path, bool postHook)
{
    m_hookIsPost = postHook;
    const bool isFlatpak = qEnvironmentVariableIsSet("FLATPAK_ID");
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

    if (isFlatpak) {
        m_hookProcess->setProgram(QStringLiteral("/usr/bin/flatpak-spawn"));
        m_hookProcess->setArguments({QStringLiteral("--host"), QStringLiteral("--watch-bus"), path});
    } else {
        m_hookProcess->setProgram(path);
        m_hookProcess->setArguments({});
    }
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
        }

        m_deviceManager = manager;

        if (m_deviceManager) {
            connect(m_deviceManager, &DeviceManager::deviceReconnected, this, &SessionRunner::onDeviceReconnected);
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
        if (m_streamManager) {
            m_streamManager->setHelperClient(client);
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

    const SessionProfile &profile = activeProfile();
    const int instanceCount = profile.instances.size();
    m_postHookArmed = true;
    inhibitScreenSaver();

    QList<int> physicalIndices;
    QList<int> streamingIndices;
    for (int i = 0; i < instanceCount; ++i) {
        if (profile.instances[i].outputMode == QStringLiteral("streaming")) {
            streamingIndices.append(i);
        } else {
            physicalIndices.append(i);
        }
    }

    m_streamingInstances.clear();
    for (int idx : streamingIndices) {
        const InstanceConfig &instConfig = profile.instances[idx];
        QVariantMap streamConfig;
        streamConfig[QStringLiteral("username")] = instConfig.username;
        streamConfig[QStringLiteral("streamResolution")] = instConfig.streamResolution;
        streamConfig[QStringLiteral("refreshRate")] = instConfig.refreshRate;
        if (!setupStreamingInstance(idx, streamConfig)) {
            beginFinalization(true, QStringLiteral("Failed to set up streaming instance %1").arg(idx + 1));
            return;
        }
    }

    const QRect screenGeometry = getScreenGeometry();
    const int physicalCount = physicalIndices.size();
    QMap<int, int> instanceToLayoutIndex;
    int layoutIdx = 0;
    for (int i = 0; i < instanceCount; ++i) {
        if (!streamingIndices.contains(i)) {
            instanceToLayoutIndex[i] = layoutIdx++;
        }
    }
    m_layouts = physicalCount > 0
        ? calculateLayout(profile.layout, physicalCount, screenGeometry, profile.gridSubLayout)
        : QList<QRect>();

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
        const bool isStreaming = streamingIndices.contains(i);
        QVariantMap config;
        config[QStringLiteral("username")] = instConfig.username;
        config[QStringLiteral("privateTemporaryDirectories")] = instConfig.privateTemporaryDirectories;
        config[QStringLiteral("monitor")] = instConfig.monitor;
        if (isStreaming) {
            const QStringList resolution = instConfig.streamResolution.split(QLatin1Char('x'));
            const int width = resolution.value(0, QStringLiteral("1920")).toInt();
            const int height = resolution.value(1, QStringLiteral("1080")).toInt();
            config[QStringLiteral("internalWidth")] = width;
            config[QStringLiteral("internalHeight")] = height;
            config[QStringLiteral("outputWidth")] = width;
            config[QStringLiteral("outputHeight")] = height;
            config[QStringLiteral("positionX")] = 0;
            config[QStringLiteral("positionY")] = 0;
            config[QStringLiteral("outputMode")] = QStringLiteral("streaming");
            config[QStringLiteral("streamResolution")] = instConfig.streamResolution;
            config[QStringLiteral("streamFps")] = instConfig.streamFps;
            config[QStringLiteral("streamBitrate")] = instConfig.streamBitrate;
            config[QStringLiteral("streamCodec")] = instConfig.streamCodec;
            config[QStringLiteral("sunshinePort")] = instConfig.sunshinePort;
            if (m_streamingInstances.contains(i)) {
                config[QStringLiteral("displayContext")] = m_streamingInstances[i].displayContext;
                if (!m_streamingInstances[i].sinkName.isEmpty()) {
                    config[QStringLiteral("sink")] = m_streamingInstances[i].sinkName;
                }
            }
        } else {
            const int layoutIndex = instanceToLayoutIndex.value(i, 0);
            config[QStringLiteral("internalWidth")] = m_layouts[layoutIndex].width();
            config[QStringLiteral("internalHeight")] = m_layouts[layoutIndex].height();
            config[QStringLiteral("outputWidth")] = m_layouts[layoutIndex].width();
            config[QStringLiteral("outputHeight")] = m_layouts[layoutIndex].height();
            config[QStringLiteral("positionX")] = m_layouts[layoutIndex].x();
            config[QStringLiteral("positionY")] = m_layouts[layoutIndex].y();
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
    if (m_finalizing) {
        if (m_hookProcess) {
            m_hookProcess->kill();
            m_hookProcess->deleteLater();
            m_hookProcess = nullptr;
            finishFinalization();
            return;
        }
        if (m_streamManager) {
            m_streamManager->stopAll();
        }
        for (auto *instance : m_instances) {
            if (instance->pid() > 0) {
                instance->stop();
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
    if (m_streamManager) {
        m_streamManager->stopAll();
    }

    for (auto *instance : m_instances) {
        if (instance->pid() > 0) {
            instance->stop();
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
    if (!m_finalizing || m_finalizationCleanupStarted || hasOwnedPids()) {
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
        if (requiresSteam && m_steamConfigManager && m_steamConfigManager->isSteamDetected()
            && (m_startingSnapshot.syncSteamShortcuts || selectedSteamShortcut)) {
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

QRect SessionRunner::getScreenGeometry() const
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        return screen->geometry();
    }

    return QRect(0, 0, 1920, 1080);
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
    } else if (layout == QStringLiteral("multi-monitor")) {
        for (int i = 0; i < instanceCount; ++i) {
            result.append(screenGeometry);
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

    Q_EMIT instanceStarted(idx);
    Q_EMIT instancesChanged();
    Q_EMIT runningInstanceCountChanged();

    if (m_streamingInstances.contains(idx)) {
        m_streamStartupErrorReported = false;
        if (!m_streamManager || idx >= m_pendingInstanceConfigs.size()
            || !m_streamManager->startStream(idx, m_pendingInstanceConfigs[idx])) {
            if (!m_finalizing && !m_streamStartupErrorReported) {
                beginFinalization(true, QStringLiteral("Failed to start streaming instance %1").arg(idx + 1));
            }
        }
        return;
    }
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

    m_windowManager->queuePositionRequest(instanceIndex, instance->gamescopePid(), targetGeometry,
                                         m_positionedWindowIds, borderless, 60000);
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
    if (!m_active || !m_hasStartingProfile || m_finalizing) {
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

    const QString &username = profile.instances[instanceIndex].username;
    QString devicePath = QStringLiteral("/dev/input/event%1").arg(eventNumber);

    if (username.isEmpty()) {
        qDebug() << "SessionRunner: Device reconnected in host session, watching:" << devicePath << "(stableId:" << stableId << ")";
        if (m_helperClient->watchDevice(devicePath)) {
            if (!m_ownedDevicePaths.contains(devicePath)) {
                m_ownedDevicePaths.append(devicePath);
            }
        }
        return;
    }

    const UserIdentity id = resolveUserIdentity(username, m_helperClient);
    if (!id.valid) {
        qWarning() << "SessionRunner: User" << username << "not found";
        return;
    }
    int uid = static_cast<int>(id.uid);

    // Prevent infinite loop: if the device node is already owned by the target user, udev has already
    // applied the custom rule and we do not need to trigger another unbind/rebind.
    QFileInfo fileInfo(devicePath);
    if (fileInfo.exists() && fileInfo.ownerId() == id.uid) {
        qDebug() << "SessionRunner: Reconnected device" << devicePath << "is already owned by" << username << "(UID:" << uid << "), skipping ownership update";
        if (devicePath.startsWith(QLatin1String("/dev/input/event"))) {
            m_helperClient->watchDevice(devicePath);
        }
        return;
    }

    qDebug() << "SessionRunner: Device reconnected, restoring ownership:" << devicePath << "(stableId:" << stableId
             << ") to user" << username;
    if (m_helperClient->setDeviceOwner(devicePath, uid)) {
        if (!m_ownedDevicePaths.contains(devicePath)) {
            m_ownedDevicePaths.append(devicePath);
        }
        qDebug() << "SessionRunner: Successfully restored ownership of" << devicePath;
    } else {
        qWarning() << "SessionRunner: Failed to restore ownership of" << devicePath;
        Q_EMIT errorOccurred(QStringLiteral("Failed to restore device ownership for %1").arg(devicePath));
    }

    QString hidrawPath = m_deviceManager->findHidrawForEvent(eventNumber);
    if (!hidrawPath.isEmpty()) {
        QFileInfo hidrawInfo(hidrawPath);
        if (hidrawInfo.exists() && hidrawInfo.ownerId() != id.uid) {
            if (m_helperClient->setDeviceOwner(hidrawPath, uid)) {
                if (!m_ownedDevicePaths.contains(hidrawPath)) {
                    m_ownedDevicePaths.append(hidrawPath);
                }
                qDebug() << "SessionRunner: Restored hidraw ownership on reconnection:" << hidrawPath;
            }
        } else {
            qDebug() << "SessionRunner: Reconnected hidraw" << hidrawPath << "is already owned by" << username << ", skipping ownership update";
        }
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

bool SessionRunner::setupStreamingInstance(int instanceIndex, const QVariantMap &config)
{
    if (!m_helperClient || !m_helperClient->isAvailable()) {
        Q_EMIT errorOccurred(QStringLiteral("Helper service required for streaming instance %1").arg(instanceIndex));
        return false;
    }

    const QString username = config.value(QStringLiteral("username")).toString();
    if (username.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Streaming instance %1 requires a username").arg(instanceIndex));
        return false;
    }

    const QString streamResolution = config.value(QStringLiteral("streamResolution")).toString();
    const QStringList resolution = streamResolution.split(QLatin1Char('x'));
    int width = resolution.size() >= 1 ? resolution[0].toInt() : 1920;
    int height = resolution.size() >= 2 ? resolution[1].toInt() : 1080;
    int refreshRate = config.value(QStringLiteral("refreshRate")).toInt();

    QString displayContext = m_helperClient->createVirtualOutput(username, width, height, refreshRate);
    if (displayContext.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to create virtual output for streaming instance %1").arg(instanceIndex));
        return false;
    }

    QString sinkName = QStringLiteral("couchplay-sunshine-%1").arg(instanceIndex);
    QString createdSinkName = m_helperClient->createNullSink(username, sinkName);
    if (createdSinkName.isEmpty()) {
        qWarning() << "SessionRunner: Failed to create null sink for streaming instance" << instanceIndex;
    }

    StreamingInstanceInfo info;
    info.username = username;
    info.displayContext = displayContext;

    info.sinkName = createdSinkName;
    info.virtualDisplayCreated = true;
    info.nullSinkCreated = !createdSinkName.isEmpty();
    m_streamingInstances[instanceIndex] = info;

    return true;
}

void SessionRunner::teardownStreamingInstances()
{
    const QList<int> indices = m_streamingInstances.keys();
    for (int idx : indices) {
        cleanupStreamingInstance(idx);
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

    const StreamingInstanceInfo &info = m_streamingInstances[index];

    if (m_helperClient && m_helperClient->isAvailable()) {
        if (info.nullSinkCreated && !info.sinkName.isEmpty()) {
            if (!m_helperClient->destroyNullSink(info.username, info.sinkName)) {
                qWarning() << "SessionRunner: Failed to destroy null sink" << info.sinkName;
            }
        }
        if (info.virtualDisplayCreated && !info.displayContext.isEmpty()) {
            if (!m_helperClient->destroyVirtualOutput(info.username, info.displayContext)) {
                qWarning() << "SessionRunner: Failed to destroy virtual output" << info.displayContext;
            }
        }
    }

    m_streamingInstances.remove(index);
}
