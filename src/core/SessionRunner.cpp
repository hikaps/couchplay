// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SessionRunner.h"
#include "../../helper/MountSpec.h"
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
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QSet>
#include <QFileInfo>
#include <QStandardPaths>

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
        Q_EMIT errorOccurred(QStringLiteral("Stream %1: %2").arg(instanceIndex).arg(error));
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
    return m_hasStartingProfile ? m_startingProfile : m_sessionManager->currentProfile();
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

    const quint64 hookGeneration = m_startupGeneration;
    QProcess *const process = new QProcess(this);
    m_hookProcess = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this,
            [this, process, hookGeneration](int exitCode, QProcess::ExitStatus exitStatus) {
        onHookFinished(process, hookGeneration, exitCode, exitStatus);
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, hookGeneration](QProcess::ProcessError error) {
        onHookError(process, hookGeneration, error);
    });

    if (isFlatpak) {
        process->setProgram(QStringLiteral("/usr/bin/flatpak-spawn"));
        process->setArguments({QStringLiteral("--host"), QStringLiteral("--watch-bus"), path});
    } else {
        process->setProgram(path);
        process->setArguments({});
    }
    process->start();
}

void SessionRunner::onHookFinished(QProcess *process,
                                   quint64 generation,
                                   int exitCode,
                                   QProcess::ExitStatus exitStatus)
{
    if (m_hookProcess != process || generation != m_startupGeneration) {
        return;
    }
    const QByteArray output = process->readAll();
    if (!output.isEmpty()) {
        qCDebug(couchplayCore) << "Session hook output:" << output.trimmed();
    }
    const bool postHook = m_hookIsPost;
    const QString program = process->program();
    process->deleteLater();
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

void SessionRunner::onHookError(QProcess *process, quint64 generation, QProcess::ProcessError error)
{
    if (m_hookProcess != process || generation != m_startupGeneration || error != QProcess::FailedToStart) {
        return;
    }
    const bool postHook = m_hookIsPost;
    const QString program = process->program();
    process->deleteLater();
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

    const quint64 startupGeneration = ++m_startupGeneration;
    const auto isCurrentStart = [this, startupGeneration] {
        return startupGeneration == m_startupGeneration && !m_finalizing;
    };

    cleanupInstances();
    if (!isCurrentStart()) {
        return false;
    }
    m_finalizing = false;
    m_startupFailure = false;
    m_preHookCompleted = false;
    m_postHookArmed = false;
    m_postHookStarted = false;
    m_finalizationMessage.clear();
    m_launchCommands.clear();

    const SessionProfile profile = m_sessionManager->currentProfile();
    const int instanceCount = profile.instances.size();
    if (instanceCount < 1) {
        Q_EMIT errorOccurred(QStringLiteral("No instances configured"));
        if (!isCurrentStart()) {
            return false;
        }
        setStatus(QStringLiteral("Error"));
        return false;
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
            if (!isCurrentStart()) {
                return false;
            }
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
        if (!isCurrentStart()) {
            return false;
        }
        if (!inGroup) {
            Q_EMIT errorOccurred(QStringLiteral("User '%1' is not a CouchPlay managed user. Please create the user via "
                                                "CouchPlay or add them to the 'couchplay' group.")
                                     .arg(username));
            if (!isCurrentStart()) {
                return false;
            }
            setStatus(QStringLiteral("Error"));
            return false;
        }
    }

    for (int i = 0; i < instanceCount; ++i) {
        const QString presetId = profile.instances[i].presetId.isEmpty()
            ? QStringLiteral("steam")
            : profile.instances[i].presetId;
        LaunchCommand command;
        if (m_presetManager) {
            command = m_presetManager->buildLaunchCommand(presetId, profile.instances[i].gameSelection);
        } else {
            command.program = QStringLiteral("steam");
            command.arguments = {QStringLiteral("-bigpicture")};
        }
        if (!isCurrentStart()) {
            return false;
        }
        if (!command.isValid()) {
            const QString message = QStringLiteral("Invalid launch command for player %1: %2")
                .arg(i + 1)
                .arg(command.errorMessage);
            Q_EMIT errorOccurred(message);
            if (!isCurrentStart()) {
                return false;
            }
            setStatus(QStringLiteral("Error"));
            return false;
        }
        m_launchCommands.append(command);
    }

    m_startingProfile = profile;
    m_hasStartingProfile = true;

    setActive(true);
    if (!isCurrentStart()) {
        return true;
    }
    setStatus(QStringLiteral("Starting session..."));
    if (!isCurrentStart()) {
        return true;
    }
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
    const quint64 startupGeneration = m_startupGeneration;
    const auto isCurrentStartup = [this, startupGeneration] {
        return startupGeneration == m_startupGeneration && m_active && !m_finalizing;
    };

    const SessionProfile &profile = activeProfile();
    const int instanceCount = profile.instances.size();
    m_postHookArmed = true;
    inhibitScreenSaver();
    if (!isCurrentStartup()) {
        return;
    }
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
        streamConfig[QStringLiteral("_startupGeneration")] = QVariant::fromValue(startupGeneration);
        const bool streamingSetupReady = setupStreamingInstance(idx, streamConfig);
        if (!isCurrentStartup()) {
            return;
        }
        if (!streamingSetupReady) {
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

    const bool deviceOwnershipReady = setupDeviceOwnership();
    if (!isCurrentStartup()) {
        return;
    }
    if (!deviceOwnershipReady) {
        beginFinalization(true, QStringLiteral("Failed to set up device ownership"));
        return;
    }


    const bool sessionResourcesReady = setupSessionResources(startupGeneration);
    if (!isCurrentStartup()) {
        return;
    }
    if (!sessionResourcesReady) {
        beginFinalization(true, QStringLiteral("Failed to set up data directories"));
        return;
    }
    const bool overrideBindsReady = buildOverrideBinds();
    if (!isCurrentStartup()) {
        return;
    }
    if (!overrideBindsReady) {
        beginFinalization(true, QStringLiteral("Failed to prepare override binds"));
        return;
    }

    m_pendingInstanceConfigs.clear();
    for (int i = 0; i < instanceCount; ++i) {
        const InstanceConfig &instConfig = profile.instances[i];
        const bool isStreaming = streamingIndices.contains(i);
        QVariantMap config;
        config[QStringLiteral("username")] = instConfig.username;
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
        config[QStringLiteral("launcherId")] = m_presetManager ? m_presetManager->getLauncherId(presetId)
                                                                  : QStringLiteral("steam");
        const LaunchCommand &command = m_launchCommands.at(i);
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
        if (m_instanceBindPaths.contains(i)) {
            config[QStringLiteral("overrideBinds")] = m_instanceBindPaths.value(i);
        }
        QStringList sharedRoots = m_instanceSharedRoots.value(i);
        if (!command.workingDirectory.isEmpty() && !sharedRoots.contains(command.workingDirectory)) {
            sharedRoots.append(command.workingDirectory);
            m_instanceSharedRoots[i] = sharedRoots;
        }
        config[QStringLiteral("sharedRoots")] = sharedRoots;
        m_pendingInstanceConfigs.append(config);
    }

    m_nextInstanceToStart = 0;
    startNextInstance();
}

void SessionRunner::beginFinalization(bool startupFailure, const QString &message)
{
    if (m_finalizing) {
        if (m_hookProcess && m_hookIsPost) {
            m_hookProcess->kill();
            m_hookProcess->deleteLater();
            m_hookProcess = nullptr;
            finishFinalization();
        }
        return;
    }

    ++m_startupGeneration;
    m_finalizing = true;
    m_startupFailure = startupFailure;
    m_finalizationMessage = message;

    if (m_hookProcess && !m_hookIsPost) {
        m_hookProcess->kill();
        m_hookProcess->deleteLater();
        m_hookProcess = nullptr;
        finishFinalization();
        return;
    }
    if (m_hookProcess && m_hookIsPost) {
        m_hookProcess->kill();
        m_hookProcess->deleteLater();
        m_hookProcess = nullptr;
        finishFinalization();
        return;
    }

    if (!m_postHookArmed && !m_sharedStateActive && m_instances.isEmpty() && m_streamingInstances.isEmpty()) {
        finishFinalization();
        return;
    }

    setStatus(QStringLiteral("Stopping session..."));
    uninhibitScreenSaver();
    if (m_streamManager) {
        m_streamManager->stopAll();
    }

    QStringList overridePaths;
    if (m_sessionManager) {
        const auto &profile = activeProfile();
        for (const auto &instConfig : profile.instances) {
            if (instConfig.overridePatterns.isEmpty() || instConfig.overrideGamePath.isEmpty()) {
                continue;
            }
            QString gameId = instConfig.gameSelection.gameId;
            if (gameId.isEmpty()) {
                gameId = QString::fromLatin1(
                    QCryptographicHash::hash(instConfig.overrideGamePath.toUtf8(), QCryptographicHash::Md5)
                        .toHex()
                        .left(16));
            }
            const QString presetId = instConfig.presetId.isEmpty() ? QStringLiteral("steam") : instConfig.presetId;
            overridePaths.append(getOverridesRootPath(presetId, gameId));
        }
    }

    for (auto *instance : m_instances) {
        if (instance->isRunning()) {
            instance->stop();
        }
    }
    restoreDeviceOwnership();
    teardownSharingState();
    teardownStreamingInstances();
    cleanupInstances();
    cleanupOverrideDirs(overridePaths);

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
    const bool wasActive = m_active;
    const QString finalStatus = startupFailure ? QStringLiteral("Error") : QStringLiteral("Stopped");
    const bool statusDidChange = m_status != finalStatus;
    const quint64 finalizationGeneration = m_startupGeneration;

    // Commit the complete stopped state before notifying observers: direct
    // signal handlers may synchronously start a new session.
    m_active = false;
    m_status = finalStatus;
    m_finalizing = false;
    m_startupFailure = false;
    m_preHookCompleted = false;
    m_postHookArmed = false;
    m_postHookStarted = false;
    m_hookIsPost = false;
    m_finalizationMessage.clear();
    m_hasStartingProfile = false;
    m_startingProfile = SessionProfile{};

    const auto isCurrentFinalization = [this, finalizationGeneration] {
        return m_startupGeneration == finalizationGeneration;
    };

    if (startupFailure) {
        if (!message.isEmpty()) {
            Q_EMIT errorOccurred(message);
            if (!isCurrentFinalization()) {
                return;
            }
        }
        Q_EMIT sessionStartFailed(message);
    } else {
        Q_EMIT sessionStopped();
    }
    if (!isCurrentFinalization()) {
        return;
    }

    if (wasActive) {
        Q_EMIT activeChanged();
        if (!isCurrentFinalization()) {
            return;
        }
    }
    if (statusDidChange) {
        Q_EMIT statusChanged();
        if (!isCurrentFinalization()) {
            return;
        }
    }
    Q_EMIT runningChanged();
    if (!isCurrentFinalization()) {
        return;
    }
    Q_EMIT instancesChanged();
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

    if (!instance->start(config, index)) {
        qWarning() << "Failed to start instance" << index;
        beginFinalization(true, QStringLiteral("Failed to start instance %1").arg(index + 1));
        return;
    }

    bool isStreamingInstance = config.value(QStringLiteral("outputMode")).toString() == QStringLiteral("streaming");
    // Streaming instances and absent window manager: start next immediately
    if (isStreamingInstance || !m_windowManager || !m_windowManager->isAvailable()) {
        ++m_nextInstanceToStart;
        startNextInstance();
    }
    // Otherwise wait for onWindowPositioned to trigger the next start
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
}

void SessionRunner::cleanupOverrideDirs(const QStringList &overridePaths)
{
    for (const QString &path : overridePaths) {
        QDir dir(path);
        if (dir.exists()) {
            if (dir.removeRecursively()) {
                qCDebug(couchplayCore) << "Cleaned up override staging directory:" << path;
            } else {
                qCWarning(couchplayCore) << "Failed to clean up override staging directory:" << path;
            }
        }
    }
}

bool SessionRunner::setupDeviceOwnership()
{
    const quint64 startupGeneration = m_startupGeneration;
    const auto isCurrentStartup = [this, startupGeneration] {
        return startupGeneration == m_startupGeneration;
    };
    if (!m_deviceManager || !m_helperClient) {
        return true;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, skipping device ownership setup";
        return true;
    }
    if (!isCurrentStartup()) {
        return false;
    }

    m_ownedDevicePaths.clear();

    if (!m_sessionManager) {
        return true;
    }
    QStringList acquiredDevicePaths;
    const auto rollbackStaleSetup = [this, &acquiredDevicePaths] {
        if (!m_helperClient || !m_helperClient->isAvailable()) {
            return;
        }
        for (const QString &path : acquiredDevicePaths) {
            if (!m_ownedDevicePaths.contains(path)) {
                m_helperClient->restoreDeviceOwner(path);
            }
        }
    };

    const auto &profile = activeProfile();
    bool allSucceeded = true;

    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        QStringList devicePaths = m_deviceManager->getDevicePathsForInstance(i);

        if (username.isEmpty()) {
            for (const QString &path : devicePaths) {
                if (path.startsWith(QLatin1String("/dev/input/event"))) {
                    const bool watched = m_helperClient->watchDevice(path);
                    if (watched && !acquiredDevicePaths.contains(path)) {
                        acquiredDevicePaths.append(path);
                    }
                    if (!isCurrentStartup()) {
                        rollbackStaleSetup();
                        return false;
                    }
                }
            }
            continue;
        }


        const UserIdentity id = resolveUserIdentity(username, m_helperClient);
        if (!isCurrentStartup()) {
            rollbackStaleSetup();
            return false;
        }
        if (!id.valid) {
            qWarning() << "SessionRunner: User" << username << "not found, skipping device ownership for instance" << i;
            continue;
        }
        int uid = static_cast<int>(id.uid);



        for (const QString &path : devicePaths) {
            const bool ownershipSet = m_helperClient->setDeviceOwner(path, uid);
            if (ownershipSet && !acquiredDevicePaths.contains(path)) {
                acquiredDevicePaths.append(path);
            }
            if (!isCurrentStartup()) {
                rollbackStaleSetup();
                return false;
            }
            if (!ownershipSet) {
                qWarning() << "SessionRunner: Failed to set ownership of" << path;
                allSucceeded = false;
                Q_EMIT errorOccurred(QStringLiteral("Failed to set device ownership for %1").arg(path));
                if (!isCurrentStartup()) {
                    rollbackStaleSetup();
                    return false;
                }
            }
        }

        QStringList hidrawPaths = m_deviceManager->getHidrawPathsForInstance(i);
        for (const QString &hidrawPath : hidrawPaths) {
            const bool ownershipSet = m_helperClient->setDeviceOwner(hidrawPath, uid);
            if (ownershipSet && !acquiredDevicePaths.contains(hidrawPath)) {
                acquiredDevicePaths.append(hidrawPath);
            }
            if (!isCurrentStartup()) {
                rollbackStaleSetup();
                return false;
            }
            if (!ownershipSet) {
                qWarning() << "SessionRunner: Failed to set hidraw ownership" << hidrawPath;
                allSucceeded = false;
            } else {
                qDebug() << "SessionRunner: Set hidraw ownership" << hidrawPath << "for user" << username;
            }
        }
    }

    if (!isCurrentStartup()) {
        rollbackStaleSetup();
        return false;
    }
    m_ownedDevicePaths = acquiredDevicePaths;
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

bool SessionRunner::setupSessionResources(quint64 startupGeneration)
{
    if (!m_helperClient || !m_sessionManager || !m_presetManager) {
        return false;
    }
    const auto isCurrentStartup = [this, startupGeneration] {
        return startupGeneration == 0
            || (startupGeneration == m_startupGeneration && m_active && !m_finalizing);
    };
    if (!isCurrentStartup()) {
        return false;
    }



    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available for session resource setup";
        return false;
    }

    // Arm the sharing-state tracker: teardown must run when the session ends,
    // including when the last game exits on its own
    m_sharedStateActive = true;
    m_steamSharedUsers.clear();
    m_instanceSharedRoots.clear();

    // Helper-first resolution: the Flatpak sandbox's getpwuid(getuid()) cannot
    // see host accounts, which would misroute home-relative copy/mount targets
    QString compositorHome = resolveCompositorHome(m_helperClient);
    if (!isCurrentStartup()) {
        return false;
    }

    const auto &profile = activeProfile();
    bool allSucceeded = true;

    for (int i = 0; i < profile.instances.size(); ++i) {
        const QString &username = profile.instances[i].username;
        const QString &presetId = profile.instances[i].presetId;

        if (username.isEmpty()) {
            continue;
        }

        LaunchPreset preset = m_presetManager->getPreset(presetId.isEmpty() ? QStringLiteral("steam") : presetId);
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
            && (m_steamConfigManager->syncShortcutsEnabled() || selectedSteamShortcut)) {
            qCDebug(couchplaySteam) << "Syncing Steam shortcuts for user" << username;
            m_steamConfigManager->loadShortcuts();
            if (!isCurrentStartup()) {
                return false;
            }
            const QStringList shortcutDirs = m_steamConfigManager->extractShortcutDirectories();
            for (const QString &dir : shortcutDirs) {
                if (QDir(dir).exists()) {
                    const bool aclSet = m_helperClient->setPathAclWithParents(dir, username);
                    if (!isCurrentStartup()) {
                        return false;
                    }
                    if (!aclSet) {
                        qCWarning(couchplaySteam) << "Failed to set ACL on shortcut directory" << dir;
                    }
                }
            }
            const bool synced = m_steamConfigManager->syncShortcutsToUser(username, isCurrentStartup);
            if (!isCurrentStartup()) {
                return false;
            }
            if (!synced) {
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
            qCDebug(couchplaySteam) << "Syncing Heroic config for user" << username;
            const bool configSynced = m_heroicConfigManager->syncConfigToUser(username);
            if (!isCurrentStartup()) {
                return false;
            }
            if (!configSynced) {
                qCWarning(couchplaySteam) << "Failed to sync Heroic config to" << username;
                allSucceeded = false;
            }
            if (m_heroicConfigManager->syncShortcutsEnabled()) {
                qCDebug(couchplaySteam) << "Syncing Heroic shortcuts for user" << username;
                const bool shortcutsSynced = m_heroicConfigManager->syncShortcutsToUser(username);
                if (!isCurrentStartup()) {
                    return false;
                }
                if (!shortcutsSynced) {
                    qCWarning(couchplaySteam) << "Failed to sync Heroic shortcuts to" << username;
                    allSucceeded = false;
                }
            }
        }

        // Prefer the instance's persisted directories (snapshotted at preset
        // selection and saved in the profile); fall back to the preset's
        // current defaults only when no snapshot was ever taken. An explicitly
        // empty snapshot must stay empty — it must not inherit later preset
        // edits.
        QList<DataDirectory> dataDirs = profile.instances[i].dataDirectories;
        if (dataDirs.isEmpty() && !profile.instances[i].dataDirectoriesSnapshotted) {
            dataDirs = preset.dataDirectories;
        }
        if (dataDirs.isEmpty()) {







            qDebug() << "SessionRunner: No data directories for instance" << i << "user" << username;
            continue;
        }
        QStringList sharedRoots;
        for (const DataDirectory &sharedDir : dataDirs) {
            if (!sharedDir.path.isEmpty() && !sharedRoots.contains(sharedDir.path)) {
                sharedRoots.append(sharedDir.path);
            }
        }
        m_instanceSharedRoots[i] = sharedRoots;

        qDebug() << "SessionRunner: Setting up" << dataDirs.size() << "data directories for user" << username;

        for (const DataDirectory &dir : dataDirs) {
            if (!isCurrentStartup()) {
                return false;
            }
            // Library sharing is opt-in: the steamRoot overlay entry is a
            // marker handled entirely by prepareDataDir (libraries are
            // alias-mounted under ~/.couchplay/steam-libs/<i> so the player's
            // own Steam root, account state and userdata stay untouched) —
            // never mount it at the home-relative path, which would shadow
            // the player's installation
            // The Steam-root overlay entry is a library-sharing marker, not a
            // generic directory. Never let a stale Steam snapshot overlay the
            // full compositor Steam root for a non-Steam launcher.
            if (dir.mode == QStringLiteral("overlay") && m_steamConfigManager
                && dir.path == m_steamConfigManager->steamPaths().steamRoot) {
                if (!requiresSteam) {
                    qCWarning(couchplaySteam) << "Ignoring Steam root data marker for non-Steam launcher" << username;
                    continue;
                }
                if (!m_steamConfigManager->shareLibraryEnabled()) {
                    qDebug() << "SessionRunner: Library sharing disabled, skipping Steam root entry for" << dir.path;
                    continue;
                }
                // Finalization writes manifests and libraryfolders.vdf
                // entries for the alias mounts — skip it when preparation
                // failed, or it would advertise libraries that never mounted.
                const bool steamLibraryPrepared =
                    m_steamConfigManager->prepareDataDir(dir, username, isCurrentStartup);
                if (!isCurrentStartup()) {
                    return false;
                }
                if (!steamLibraryPrepared) {
                    qCWarning(couchplaySteam) << "Steam library sharing failed for" << dir.path;
                    allSucceeded = false;
                    continue;
                }
                m_steamSharedUsers.insert(username);
                if (!m_steamConfigManager->finalizeDataDir(dir, username)) {
                    qCWarning(couchplaySteam) << "Steam library finalize failed for" << dir.path;
                    allSucceeded = false;
                }
                if (!isCurrentStartup()) {
                    return false;
                }
                continue;
            }

            // Stale snapshots may carry the heroic config root as a copy dir;
            // config sync above replaces the generic whole-directory copy
            if (dir.mode == QStringLiteral("copy") && requiresHeroic && m_heroicConfigManager
                && dir.path == m_heroicConfigManager->configPath()) {
                qDebug() << "SessionRunner: Heroic config handled by config sync, skipping copy of" << dir.path;
                continue;
            }

            if (requiresSteam && m_steamConfigManager) {
                const bool steamDataPrepared =
                    m_steamConfigManager->prepareDataDir(dir, username, isCurrentStartup);
                if (!isCurrentStartup()) {
                    return false;
                }
                if (!steamDataPrepared) {
                    qCWarning(couchplaySteam) << "Steam prepareDataDir failed for" << dir.path;
                    allSucceeded = false;
                }
            }

            QString playerViewRelative; // where the player sees this directory
            if (dir.mode == QStringLiteral("copy")) {
                QString relativePath;
                if (dir.path.startsWith(compositorHome + QLatin1Char('/'))) {
                    relativePath = dir.path.mid(compositorHome.length() + 1);
                } else {
                    // External sources have no home-relative location; map the
                    // full normalized path under .couchplay/copies — a basename
                    // alone would collide (/mnt/a/save vs /media/b/save) and
                    // replacement semantics would delete the first copy
                    relativePath = QStringLiteral(".couchplay/copies/")
                        + dataDirectoryStagingSlug(dir.path, compositorHome);
                }
                const bool copied = m_helperClient->copyDirectoryToUser(username, dir.path, relativePath);
                if (!isCurrentStartup()) {
                    return false;
                }
                if (!copied) {
                    qWarning() << "SessionRunner: Failed to copy directory" << dir.path << "for user" << username;
                    allSucceeded = false;
                } else {
                    playerViewRelative = relativePath;
                }
            } else if (dir.mode == QStringLiteral("overlay") || dir.mode == QStringLiteral("bind")) {
                // Both mount at the player's home-relative equivalent path
                // (external paths land under .couchplay/mounts), mirroring
                // computeMountTarget's empty-alias mapping. The player view is
                // only recorded on success: mirroring staged data into a
                // failed mount's plain target directory would put files where
                // a later successful mount would hide them.
                QString relativePath;
                if (dir.path.startsWith(compositorHome + QLatin1Char('/'))) {
                    relativePath = dir.path.mid(compositorHome.length() + 1);
                } else {
                    relativePath = QStringLiteral(".couchplay/mounts") + dir.path;
                }
                bool mounted = false;
                if (dir.mode == QStringLiteral("overlay")) {
                    mounted = m_helperClient->setupOverlayMount(username, dir.path, QString());
                    if (!mounted) {
                        qWarning() << "SessionRunner: Failed to setup overlay mount for" << dir.path
                                   << "user" << username;
                        allSucceeded = false;
                    }
                } else {
                    // Escaped spec: paths containing '|' or '\' survive the
                    // helper's source|alias wire format
                    const QStringList dirSpec = {encodeMountSpec(dir.path, QString())};
                    mounted = m_helperClient->mountSharedDirectories(username, dirSpec) >= 1;
                    if (!mounted) {
                        qWarning() << "SessionRunner: Failed to bind mount" << dir.path << "for user" << username;
                        allSucceeded = false;
                    }
                }
                if (mounted) {
                    playerViewRelative = relativePath;
                }
            } else if (dir.mode == QStringLiteral("acl")) {
                const bool parentsOk = m_helperClient->setPathAclWithParents(dir.path, username);
                if (!isCurrentStartup()) {
                    return false;
                }
                const bool contentsOk = m_helperClient->setDirectoryAcl(dir.path, username, true);
                if (!parentsOk || !contentsOk) {
                    qWarning() << "SessionRunner: Failed to set recursive ACL for" << dir.path << "user" << username;
                    allSucceeded = false;
                }
            }

            if (!isCurrentStartup()) {
                return false;
            }
            // Merge hand-staged per-player files into the player's view (after
            // the mount/copy). Only copy and overlay qualify: both give the
            // player a private tree, while bind mounts have no upper layer —
            // mirroring into a bind mount would mutate and re-own the shared
            // source itself.
            if (!playerViewRelative.isEmpty()
                && (dir.mode == QStringLiteral("copy") || dir.mode == QStringLiteral("overlay"))) {
                const QString stagingDir = playerDataStagingRoot(presetId.isEmpty() ? QStringLiteral("steam") : presetId,
                                                                 username)
                    + QLatin1Char('/') + dataDirectoryStagingSlug(dir.path, compositorHome);
                const QDir staging(stagingDir);
                if (staging.exists() && !staging.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
                    qCDebug(couchplaySharing) << "Mirroring staged data" << stagingDir << "for user" << username;
                    if (!m_helperClient->mirrorDirectoryContents(username, stagingDir, playerViewRelative)) {
                        qWarning() << "SessionRunner: Failed to mirror staged data" << stagingDir;
                        allSucceeded = false;
                    }
                }
            }

            if (!isCurrentStartup()) {
                return false;
            }
            if (requiresSteam && m_steamConfigManager) {
                if (!m_steamConfigManager->finalizeDataDir(dir, username)) {
                    qCWarning(couchplaySteam) << "Steam finalizeDataDir failed for" << dir.path;
                    allSucceeded = false;
                }
            }
        }
    }

    return allSucceeded;
}

void SessionRunner::teardownSharedDirectories()
{
    if (!m_helperClient) {
        return;
    }

    if (!m_helperClient->isAvailable()) {
        qWarning() << "SessionRunner: Helper not available, cannot unmount shared directories";
        return;
    }

    m_helperClient->unmountAllSharedDirectories();
}

void SessionRunner::teardownSharingState()
{
    // Release the privileged sharing state exactly once per session: shared
    // mounts plus per-player Steam library sharing. Invoked from stop() and
    // from the natural-exit path (last game exited on its own) — without the
    // latter, mounts persist for the helper's lifetime and the next session
    // stacks on top or fails reusing the same overlay work directory.
    if (!m_sharedStateActive) {
        return;
    }
    m_sharedStateActive = false;

    teardownSharedDirectories();

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

bool SessionRunner::buildOverrideBinds()
{
    m_instanceBindPaths.clear();

    if (!m_sessionManager) {
        return true;
    }

    const auto &profile = activeProfile();

    for (int i = 0; i < profile.instances.size(); ++i) {
        const auto &instConfig = profile.instances[i];

        if (instConfig.overridePatterns.isEmpty()) {
            qCDebug(couchplaySharing) << "No override patterns for instance" << i << "- skipping bind paths";
            continue;
        }
        qCDebug(couchplaySharing) << "Building bind paths for instance" << i << "with"
                                  << instConfig.overridePatterns.size() << "patterns";

        const QString &username = instConfig.username;
        if (username.isEmpty()) {
            qCDebug(couchplaySharing) << "Instance" << i << "has no username, skipping bind paths";
            continue;
        }

        QString gamePath = instConfig.overrideGamePath;
        if (gamePath.isEmpty()) {
            qCWarning(couchplaySharing) << "Instance" << i << "has patterns but no game path, skipping";
            continue;
        }

        QString gameId = instConfig.gameSelection.gameId;
        if (gameId.isEmpty()) {
            gameId = QString::fromLatin1(
                QCryptographicHash::hash(gamePath.toUtf8(), QCryptographicHash::Md5).toHex().left(16));
        }

        QStringList matchedFiles = expandPatternsToFiles(gamePath, instConfig.overridePatterns);
        qCDebug(couchplaySharing) << "Instance" << i << "matched" << matchedFiles.size() << "files from"
                                  << instConfig.overridePatterns.size() << "patterns";

        QString presetId = instConfig.presetId;
        if (presetId.isEmpty()) {
            presetId = QStringLiteral("steam"); // Default preset
        }
        QString overridesRoot = getOverridesRootPath(presetId, gameId);

        // Bind format: "<stagingDir>/<relativeFile>:<gamePath>/<relativeFile>"
        QStringList bindPaths;
        QStringList sharedRoots = m_instanceSharedRoots.value(i);
        if (!sharedRoots.contains(gamePath)) {
            sharedRoots.append(gamePath);
        }
        if (!sharedRoots.contains(overridesRoot)) {
            sharedRoots.append(overridesRoot);
        }
        m_instanceSharedRoots[i] = sharedRoots;
        for (const QString &relativePath : matchedFiles) {
            QString bindEntry =
                overridesRoot + relativePath + QLatin1Char(':') + gamePath + QLatin1Char('/') + relativePath;
            bindPaths.append(bindEntry);
        }

        if (!bindPaths.isEmpty()) {
            m_instanceBindPaths[i] = bindPaths;
            qCDebug(couchplaySharing) << "Instance" << i << "has" << bindPaths.size() << "bind paths";
        }

        loadOverrideFiles(overridesRoot, matchedFiles, username, gameId);
    }

    return true;
}

QRect SessionRunner::getScreenGeometry() const
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        return screen->geometry();
    }

    return QRect(0, 0, 1920, 1080);
}

QString SessionRunner::getOverridesRootPath(const QString &presetId, const QString &gameKeyHash)
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString path =
        basePath + QStringLiteral("/overrides/") + presetId + QStringLiteral("/") + gameKeyHash + QStringLiteral("/");
    return path;
}

QString SessionRunner::getAndEnsureOverridesPath(const QString &presetId)
{
    QString basePath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString overridesPath = basePath + QStringLiteral("/overrides/") + presetId + QStringLiteral("/");

    QDir dir;
    if (!dir.exists(overridesPath)) {
        dir.mkpath(overridesPath);
    }

    return overridesPath;
}

QStringList SessionRunner::expandPatternsToFiles(const QString &gamePath, const QStringList &patterns)
{
    QStringList matchedFiles;

    if (gamePath.isEmpty() || patterns.isEmpty()) {
        return matchedFiles;
    }

    QDir baseDir(gamePath);
    if (!baseDir.exists()) {
        qCWarning(couchplaySharing) << "expandPatternsToFiles: gamePath does not exist:" << gamePath;
        return matchedFiles;
    }

    QDirIterator it(gamePath, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);

    while (it.hasNext()) {
        QString filePath = it.next();
        QString relativePath = baseDir.relativeFilePath(filePath);

        for (const QString &pattern : patterns) {
            if (QDir::match(pattern, relativePath)) {
                matchedFiles.append(relativePath);
                qCDebug(couchplaySharing) << "Pattern" << pattern << "matched file:" << relativePath;
                break;
            }
        }
    }

    qCDebug(couchplaySharing) << "expandPatternsToFiles:" << matchedFiles.size() << "files matched from"
                              << patterns.size() << "patterns in" << gamePath;

    return matchedFiles;
}

void SessionRunner::loadOverrideFiles(const QString &overridesRoot,
                                      const QStringList &matchedFiles,
                                      const QString &username,
                                      const QString &gameId)
{
    Q_UNUSED(username)
    Q_UNUSED(gameId)

    if (overridesRoot.isEmpty() || matchedFiles.isEmpty()) {
        return;
    }

    QDir dir(overridesRoot);
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            qCWarning(couchplaySharing) << "Failed to create overrides directory:" << overridesRoot;
            return;
        }
    }

    qCDebug(couchplaySharing) << "Override staging ready:" << overridesRoot << "with" << matchedFiles.size()
                              << "files for bind paths";
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
    if (instance) {
        int idx = instance->index();

        if (m_streamingInstances.contains(idx)) {
            if (idx < m_pendingInstanceConfigs.size()) {
                m_streamManager->startStream(idx, m_pendingInstanceConfigs[idx]);
            }
        } else {
            positionInstanceWindow(instance);
        }

        Q_EMIT instanceStarted(idx);
        Q_EMIT instancesChanged();
        Q_EMIT runningInstanceCountChanged();
    }
}

void SessionRunner::onInstanceStopped()
{
    auto *instance = qobject_cast<GamescopeInstance *>(sender());
    if (!instance || !m_instances.contains(instance)) {
        return;
    }

    const quint64 stopGeneration = m_startupGeneration;
    const int idx = instance->index();
    if (m_streamingInstances.contains(idx)) {
        m_streamManager->stopStream(idx);
        if (stopGeneration != m_startupGeneration) {
            return;
        }
        cleanupStreamingInstance(idx);
        if (stopGeneration != m_startupGeneration) {
            return;
        }
    }

    Q_EMIT instanceStopped(idx);
    if (stopGeneration != m_startupGeneration) {
        return;
    }
    Q_EMIT instancesChanged();
    if (stopGeneration != m_startupGeneration) {
        return;
    }
    Q_EMIT runningInstanceCountChanged();
    if (stopGeneration != m_startupGeneration) {
        return;
    }

    if (!m_finalizing && !isRunning()) {
        beginFinalization(false);
    }
}

void SessionRunner::onInstanceError(const QString &message)
{
    auto *instance = qobject_cast<GamescopeInstance *>(sender());
    QString fullMessage;
    if (instance) {
        fullMessage = QStringLiteral("Instance %1: %2").arg(instance->index()).arg(message);
    } else {
        fullMessage = message;
    }
    Q_EMIT errorOccurred(fullMessage);
}

void SessionRunner::positionInstanceWindow(GamescopeInstance *instance)
{
    if (!instance || !m_windowManager || !m_windowManager->isAvailable()) {
        return;
    }

    QRect targetGeometry = instance->windowGeometry();
    int instanceIndex = instance->index();

    const bool borderless = m_settingsManager && m_settingsManager->borderlessWindows();

    m_windowManager->queuePositionRequest(instanceIndex, targetGeometry, m_positionedWindowIds, borderless, 60000);
}

void SessionRunner::onWindowPositioned(int requestId, const QString &windowId)
{
    Q_UNUSED(requestId)
    if (!m_positionedWindowIds.contains(windowId)) {
        m_positionedWindowIds.append(windowId);
    }

    // Trigger next sequential instance launch
    if (!m_pendingInstanceConfigs.isEmpty()) {
        ++m_nextInstanceToStart;
        startNextInstance();
    }
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
    if (!isRunning()) {
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
    CouchPlayHelperClient *const helperClient = m_helperClient;
    const quint64 startupGeneration = config.value(QStringLiteral("_startupGeneration")).toULongLong();
    const auto isCurrentStartup = [this, startupGeneration] {
        return startupGeneration == 0
            || (startupGeneration == m_startupGeneration && m_active && !m_finalizing);
    };
    if (!helperClient || !helperClient->isAvailable()) {
        Q_EMIT errorOccurred(QStringLiteral("Helper service required for streaming instance %1").arg(instanceIndex));
        return false;
    }

    const QString username = config.value(QStringLiteral("username")).toString();
    const auto destroyLocalResources = [helperClient, &username](const QString &displayContext, const QString &sinkName) {
        if (!helperClient || !helperClient->isAvailable()) {
            return;
        }
        if (!sinkName.isEmpty() && !helperClient->destroyNullSink(username, sinkName)) {
            qWarning() << "SessionRunner: Failed to destroy stale null sink" << sinkName;
        }
        if (!displayContext.isEmpty() && !helperClient->destroyVirtualOutput(username, displayContext)) {
            qWarning() << "SessionRunner: Failed to destroy stale virtual output" << displayContext;
        }
    };
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
    if (!isCurrentStartup()) {
        destroyLocalResources(displayContext, QString());
        return false;
    }
    if (displayContext.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to create virtual output for streaming instance %1").arg(instanceIndex));
        return false;
    }

    QString sinkName = QStringLiteral("couchplay-sunshine-%1").arg(instanceIndex);
    QString createdSinkName = m_helperClient->createNullSink(username, sinkName);
    if (!isCurrentStartup()) {
        destroyLocalResources(displayContext, createdSinkName);
        return false;
    }
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
    if (!m_streamingInstances.contains(index)) {
        return;
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
