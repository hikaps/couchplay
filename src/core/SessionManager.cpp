// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "SessionManager.h"
#include "Logging.h"
#include "SessionRunner.h"

#include <QDesktopServices>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QUrl>

#include <unistd.h>
#include <pwd.h>

SessionManager::SessionManager(QObject *parent)
    : QObject(parent)
{
    QDir().mkpath(profilesDir());
    newSession();
    refreshProfiles();
}

void SessionManager::setPresetManager(PresetManager *manager)
{
    if (m_presetManager != manager) {
        m_presetManager = manager;
        Q_EMIT presetManagerChanged();
    }
}

QVariantList InstanceConfig::dataDirectoriesAsVariant() const
{
    QVariantList list;
    for (const DataDirectory &dir : dataDirectories) {
        QVariantMap dirMap;
        dirMap[QStringLiteral("path")] = dir.path;
        dirMap[QStringLiteral("mode")] = dir.mode;
        list.append(dirMap);
    }
    return list;
}

void InstanceConfig::setDataDirectoriesFromVariant(const QVariantList &dirs)
{
    dataDirectories.clear();
    for (const QVariant &var : dirs) {
        DataDirectory dir = DataDirectory::fromVariant(var);
        if (!dir.path.isEmpty()) {
            dataDirectories.append(dir);
        }
    }
    dataDirectoriesSnapshotted = true;
}

QString SessionManager::profilesDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/profiles");
}

SessionManager::~SessionManager() = default;

QString SessionManager::profilePath(const QString &name) const
{
    return profilesDir() + QStringLiteral("/") + name + QStringLiteral(".conf");
}

void SessionManager::newSession()
{
    m_currentProfile = SessionProfile();
    m_currentProfile.name = QString();
    m_currentProfile.layout = QStringLiteral("horizontal");

    m_currentProfile.instances.clear();
    for (int i = 0; i < 2; ++i) {
        InstanceConfig config;
        config.monitor = 0;
        m_currentProfile.instances.append(config);
    }

    Q_EMIT currentProfileChanged();
    Q_EMIT currentLayoutChanged();
    Q_EMIT instanceCountChanged();
    Q_EMIT instancesChanged();
}

void SessionManager::refreshProfiles()
{
    m_savedProfiles.clear();

    QDir dir(profilesDir());
    QStringList filters;
    filters << QStringLiteral("*.conf");

    for (const QString &fileName : dir.entryList(filters, QDir::Files)) {
        QString name = fileName;
        name.chop(5); // Remove ".conf"

        SessionProfile profile;
        profile.name = name;
        profile.filePath = dir.absoluteFilePath(fileName);

        // Read basic info
        KConfig config(profile.filePath);
        KConfigGroup general = config.group(QStringLiteral("General"));
        profile.layout = general.readEntry("layout", QStringLiteral("horizontal"));

        m_savedProfiles.append(profile);
    }

    Q_EMIT savedProfilesChanged();
}

bool SessionManager::saveProfile(const QString &name)
{
    if (name.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Profile name cannot be empty"));
        return false;
    }

    QString path = profilePath(name);
    KConfig config(path);

    // General section
    KConfigGroup general = config.group(QStringLiteral("General"));
    general.writeEntry("name", name);
    general.writeEntry("layout", m_currentProfile.layout);
    general.writeEntry("gridSubLayout", m_currentProfile.gridSubLayout);
    general.writeEntry("instanceCount", m_currentProfile.instances.size());

    for (int i = 0; i < m_currentProfile.instances.size(); ++i) {
        const InstanceConfig &inst = m_currentProfile.instances[i];
        KConfigGroup instGroup = config.group(QStringLiteral("Instance%1").arg(i));

        instGroup.writeEntry("username", inst.username);
        instGroup.writeEntry("monitor", inst.monitor);
        instGroup.writeEntry("internalWidth", inst.internalWidth);
        instGroup.writeEntry("internalHeight", inst.internalHeight);
        instGroup.writeEntry("outputWidth", inst.outputWidth);
        instGroup.writeEntry("outputHeight", inst.outputHeight);
        instGroup.writeEntry("refreshRate", inst.refreshRate);
        instGroup.writeEntry("scalingMode", inst.scalingMode);
        instGroup.writeEntry("filterMode", inst.filterMode);
        instGroup.writeEntry("gameCommand", inst.gameCommand);
        instGroup.writeEntry("steamAppId", inst.steamAppId);
        instGroup.writeEntry("presetId", inst.presetId);
        // Only persist a snapshot when one was taken: writing an empty
        // dataDirectories key for an unsnapshotted instance would turn
        // "use preset defaults" into "explicitly no directories" after a
        // save/load round-trip
        if (inst.dataDirectoriesSnapshotted) {
            QStringList dirEntries;
            for (const DataDirectory &dir : inst.dataDirectories) {
                dirEntries.append(dir.path + QLatin1Char('|') + dir.mode);
            }
            instGroup.writeEntry("dataDirectories", dirEntries.join(QLatin1Char('\n')));
        } else {
            instGroup.deleteEntry("dataDirectories");
            instGroup.deleteEntry("sharedDirectories");
        }
        instGroup.writeEntry("overrideGamePath", inst.overrideGamePath);
        instGroup.writeEntry("overrideFiles", inst.overrideFiles);
        instGroup.writeEntry("overridePatterns", inst.overridePatterns);
        instGroup.writeEntry("outputMode", inst.outputMode);
        instGroup.writeEntry("streamResolution", inst.streamResolution);
        instGroup.writeEntry("streamFps", inst.streamFps);
        instGroup.writeEntry("streamBitrate", inst.streamBitrate);
        instGroup.writeEntry("streamCodec", inst.streamCodec);
        instGroup.writeEntry("sunshinePort", inst.sunshinePort);

        // Convert devices to string list for backwards compatibility
        QStringList deviceStrings;
        for (int dev : inst.devices) {
            deviceStrings << QString::number(dev);
        }
        instGroup.writeEntry("devices", deviceStrings);
        instGroup.writeEntry("deviceStableIds", inst.deviceStableIds);
        instGroup.writeEntry("deviceStableIdNames", inst.deviceStableIdNames);
    }

    config.sync();

    m_currentProfile.name = name;
    m_currentProfile.filePath = path;

    Q_EMIT currentProfileChanged();
    refreshProfiles();

    return true;
}

bool SessionManager::loadProfile(const QString &name)
{
    QString path = profilePath(name);

    if (!QFile::exists(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Profile not found: %1").arg(name));
        return false;
    }

    KConfig config(path);

    KConfigGroup general = config.group(QStringLiteral("General"));
    m_currentProfile.name = name;
    m_currentProfile.filePath = path;
    m_currentProfile.layout = general.readEntry("layout", QStringLiteral("horizontal"));
    m_currentProfile.gridSubLayout = general.readEntry("gridSubLayout", QString());
    int instanceCount = general.readEntry("instanceCount", 2);

    m_currentProfile.instances.clear();
    for (int i = 0; i < instanceCount; ++i) {
        KConfigGroup instGroup = config.group(QStringLiteral("Instance%1").arg(i));

        InstanceConfig inst;
        inst.username = instGroup.readEntry("username", QString());
        inst.monitor = instGroup.readEntry("monitor", 0);
        inst.internalWidth = instGroup.readEntry("internalWidth", 1920);
        inst.internalHeight = instGroup.readEntry("internalHeight", 1080);
        inst.outputWidth = instGroup.readEntry("outputWidth", 960);
        inst.outputHeight = instGroup.readEntry("outputHeight", 1080);
        inst.refreshRate = instGroup.readEntry("refreshRate", 60);
        inst.scalingMode = instGroup.readEntry("scalingMode", QStringLiteral("fit"));
        inst.filterMode = instGroup.readEntry("filterMode", QStringLiteral("linear"));
        inst.gameCommand = instGroup.readEntry("gameCommand", QString());
        inst.steamAppId = instGroup.readEntry("steamAppId", QString());
        inst.presetId = instGroup.readEntry("presetId", QStringLiteral("steam"));
        if (instGroup.hasKey("dataDirectories")) {
            // New format: newline-separated "path|mode" entries. The key's
            // presence marks a taken snapshot — an explicitly empty list stays
            // empty (no preset-default fallback at session start).
            inst.dataDirectoriesSnapshotted = true;
            QString dataDirsRaw = instGroup.readEntry("dataDirectories", QString());
            const QStringList entries = dataDirsRaw.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &entry : entries) {
                int pipePos = entry.indexOf(QLatin1Char('|'));
                if (pipePos > 0) {
                    DataDirectory dir;
                    dir.path = entry.left(pipePos);
                    dir.mode = entry.mid(pipePos + 1);
                    if (dir.mode.isEmpty()) {
                        dir.mode = QStringLiteral("acl");
                    }
                    inst.dataDirectories.append(dir);
                }
            }
        } else if (instGroup.hasKey("sharedDirectories")) {
            // Legacy: sharedDirectories was a plain QStringList (paths only) that
            // got bind-mounted at the player's home-relative equivalent path —
            // migrate as bind to preserve that visibility
            inst.dataDirectoriesSnapshotted = true;
            QStringList legacyDirs = instGroup.readEntry("sharedDirectories", QStringList());
            for (const QString &path : legacyDirs) {
                DataDirectory dir;
                dir.path = path;
                dir.mode = QStringLiteral("bind");
                inst.dataDirectories.append(dir);
            }
        }
        inst.overrideGamePath =
            instGroup.readEntry("overrideGamePath", instGroup.readEntry("overlayGamePath", QString()));
        inst.overrideFiles = instGroup.readEntry("overrideFiles", QStringList());

        inst.overridePatterns =
            instGroup.readEntry("overridePatterns", instGroup.readEntry("overlayPatterns", QStringList()));

        // Migration: copy legacy overrideFiles to overridePatterns if overridePatterns is empty
        if (inst.overridePatterns.isEmpty() && !inst.overrideFiles.isEmpty()) {
            inst.overridePatterns = inst.overrideFiles;
            qDebug() << "Migrated overrideFiles to overridePatterns for instance" << i;
        }

        inst.outputMode = instGroup.readEntry("outputMode", QStringLiteral("physical"));
        inst.streamResolution = instGroup.readEntry("streamResolution", QStringLiteral("1920x1080"));
        inst.streamFps = instGroup.readEntry("streamFps", 60);
        inst.streamBitrate = instGroup.readEntry("streamBitrate", 20000);
        inst.streamCodec = instGroup.readEntry("streamCodec", QStringLiteral("h264"));
        inst.sunshinePort = instGroup.readEntry("sunshinePort", 47989);

        inst.deviceStableIds = instGroup.readEntry("deviceStableIds", QStringList());
        inst.deviceStableIdNames = instGroup.readEntry("deviceStableIdNames", QStringList());

        QStringList deviceStrings = instGroup.readEntry("devices", QStringList());
        for (const QString &devStr : deviceStrings) {
            inst.devices << devStr.toInt();
        }

        m_currentProfile.instances.append(inst);
    }

    Q_EMIT currentProfileChanged();
    Q_EMIT currentLayoutChanged();
    Q_EMIT instanceCountChanged();
    Q_EMIT instancesChanged();

    QVariantMap deviceInfoByInstance;
    for (int i = 0; i < m_currentProfile.instances.size(); ++i) {
        const QStringList &stableIds = m_currentProfile.instances[i].deviceStableIds;
        const QStringList &names = m_currentProfile.instances[i].deviceStableIdNames;
        if (!stableIds.isEmpty()) {
            QVariantMap instanceInfo;
            instanceInfo[QStringLiteral("stableIds")] = QVariant::fromValue(stableIds);
            instanceInfo[QStringLiteral("names")] = QVariant::fromValue(names);
            deviceInfoByInstance.insert(QString::number(i), instanceInfo);
        }
    }
    if (!deviceInfoByInstance.isEmpty()) {
        Q_EMIT profileLoaded(deviceInfoByInstance);
    }

    return true;
}

bool SessionManager::deleteProfile(const QString &name)
{
    QString path = profilePath(name);

    if (!QFile::exists(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Profile not found: %1").arg(name));
        return false;
    }

    if (!QFile::remove(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to delete profile: %1").arg(name));
        return false;
    }

    if (m_currentProfile.name == name) {
        m_currentProfile.name = QString();
        m_currentProfile.filePath = QString();
        Q_EMIT currentProfileChanged();
    }

    refreshProfiles();
    return true;
}

void SessionManager::setCurrentLayout(const QString &layout)
{
    if (m_currentProfile.layout != layout) {
        m_currentProfile.layout = layout;
        if (layout != QStringLiteral("grid")) {
            m_currentProfile.gridSubLayout.clear();
            Q_EMIT currentGridSubLayoutChanged();
        }
        Q_EMIT currentLayoutChanged();
    }
}

void SessionManager::setCurrentGridSubLayout(const QString &subLayout)
{
    if (m_currentProfile.gridSubLayout != subLayout) {
        m_currentProfile.gridSubLayout = subLayout;
        Q_EMIT currentGridSubLayoutChanged();
    }
}

void SessionManager::setInstanceCount(int count)
{
    if (count < 2)
        count = 2;
    if (count > 4)
        count = 4;

    if (m_currentProfile.instances.size() == count) {
        return;
    }

    while (m_currentProfile.instances.size() < count) {
        InstanceConfig config;
        config.monitor = 0;
        m_currentProfile.instances.append(config);
    }

    while (m_currentProfile.instances.size() > count) {
        m_currentProfile.instances.removeLast();
    }

    Q_EMIT instanceCountChanged();
    Q_EMIT instancesChanged();
}

QVariantMap SessionManager::getInstanceConfig(int index) const
{
    QVariantMap map;

    if (index < 0 || index >= m_currentProfile.instances.size()) {
        return map;
    }

    const InstanceConfig &inst = m_currentProfile.instances[index];
    map[QStringLiteral("username")] = inst.username;
    map[QStringLiteral("monitor")] = inst.monitor;
    map[QStringLiteral("internalWidth")] = inst.internalWidth;
    map[QStringLiteral("internalHeight")] = inst.internalHeight;
    map[QStringLiteral("outputWidth")] = inst.outputWidth;
    map[QStringLiteral("outputHeight")] = inst.outputHeight;
    map[QStringLiteral("refreshRate")] = inst.refreshRate;
    map[QStringLiteral("scalingMode")] = inst.scalingMode;
    map[QStringLiteral("filterMode")] = inst.filterMode;
    map[QStringLiteral("gameCommand")] = inst.gameCommand;
    map[QStringLiteral("steamAppId")] = inst.steamAppId;
    map[QStringLiteral("presetId")] = inst.presetId;
    map[QStringLiteral("overridePatterns")] = inst.overridePatterns;
    QVariantList dataDirsVariant;
    for (const DataDirectory &dir : inst.dataDirectories) {
        QVariantMap dirMap;
        dirMap[QStringLiteral("path")] = dir.path;
        dirMap[QStringLiteral("mode")] = dir.mode;
        dataDirsVariant.append(dirMap);
    }
    map[QStringLiteral("dataDirectories")] = dataDirsVariant;
    map[QStringLiteral("dataDirectoriesSnapshotted")] = inst.dataDirectoriesSnapshotted;
    map[QStringLiteral("outputMode")] = inst.outputMode;
    map[QStringLiteral("streamResolution")] = inst.streamResolution;
    map[QStringLiteral("streamFps")] = inst.streamFps;
    map[QStringLiteral("streamBitrate")] = inst.streamBitrate;
    map[QStringLiteral("streamCodec")] = inst.streamCodec;
    map[QStringLiteral("sunshinePort")] = inst.sunshinePort;

    QVariantList deviceList;
    for (int dev : inst.devices) {
        deviceList << dev;
    }
    map[QStringLiteral("devices")] = deviceList;

    QVariantList stableIdList;
    for (const QString &id : inst.deviceStableIds) {
        stableIdList << id;
    }
    map[QStringLiteral("deviceStableIds")] = stableIdList;

    QVariantList stableIdNameList;
    for (const QString &name : inst.deviceStableIdNames) {
        stableIdNameList << name;
    }
    map[QStringLiteral("deviceStableIdNames")] = stableIdNameList;

    return map;
}

void SessionManager::setInstanceConfig(int index, const QVariantMap &config)
{
    if (index < 0 || index >= m_currentProfile.instances.size()) {
        return;
    }

    InstanceConfig &inst = m_currentProfile.instances[index];

    if (config.contains(QStringLiteral("username")))
        inst.username = config[QStringLiteral("username")].toString();
    if (config.contains(QStringLiteral("monitor")))
        inst.monitor = config[QStringLiteral("monitor")].toInt();
    if (config.contains(QStringLiteral("internalWidth")))
        inst.internalWidth = config[QStringLiteral("internalWidth")].toInt();
    if (config.contains(QStringLiteral("internalHeight")))
        inst.internalHeight = config[QStringLiteral("internalHeight")].toInt();
    if (config.contains(QStringLiteral("outputWidth")))
        inst.outputWidth = config[QStringLiteral("outputWidth")].toInt();
    if (config.contains(QStringLiteral("outputHeight")))
        inst.outputHeight = config[QStringLiteral("outputHeight")].toInt();
    if (config.contains(QStringLiteral("refreshRate")))
        inst.refreshRate = config[QStringLiteral("refreshRate")].toInt();
    if (config.contains(QStringLiteral("scalingMode")))
        inst.scalingMode = config[QStringLiteral("scalingMode")].toString();
    if (config.contains(QStringLiteral("filterMode")))
        inst.filterMode = config[QStringLiteral("filterMode")].toString();
    if (config.contains(QStringLiteral("gameCommand")))
        inst.gameCommand = config[QStringLiteral("gameCommand")].toString();
    if (config.contains(QStringLiteral("steamAppId")))
        inst.steamAppId = config[QStringLiteral("steamAppId")].toString();
    if (config.contains(QStringLiteral("presetId")))
        inst.presetId = config[QStringLiteral("presetId")].toString();
    if (config.contains(QStringLiteral("overridePatterns")))
        inst.overridePatterns = config[QStringLiteral("overridePatterns")].toStringList();
    if (config.contains(QStringLiteral("overrideGamePath")))
        inst.overrideGamePath = config[QStringLiteral("overrideGamePath")].toString();
    if (config.contains(QStringLiteral("outputMode")))
        inst.outputMode = config[QStringLiteral("outputMode")].toString();
    if (config.contains(QStringLiteral("streamResolution")))
        inst.streamResolution = config[QStringLiteral("streamResolution")].toString();
    if (config.contains(QStringLiteral("streamFps")))
        inst.streamFps = config[QStringLiteral("streamFps")].toInt();
    if (config.contains(QStringLiteral("streamBitrate")))
        inst.streamBitrate = config[QStringLiteral("streamBitrate")].toInt();
    if (config.contains(QStringLiteral("streamCodec")))
        inst.streamCodec = config[QStringLiteral("streamCodec")].toString();
    if (config.contains(QStringLiteral("sunshinePort")))
        inst.sunshinePort = config[QStringLiteral("sunshinePort")].toInt();
    if (config.contains(QStringLiteral("dataDirectories"))) {
        const QVariantList dirs = config[QStringLiteral("dataDirectories")].toList();
        QList<DataDirectory> dataDirs;
        for (const QVariant &var : dirs) {
            DataDirectory dir = DataDirectory::fromVariant(var);
            if (!dir.path.isEmpty()) {
                dataDirs.append(dir);
            }
        }
        inst.dataDirectories = dataDirs;
        inst.dataDirectoriesSnapshotted = true;
    }

    Q_EMIT instancesChanged();

    if (!m_currentProfile.name.isEmpty()) {
        saveProfile(m_currentProfile.name);
    }
}

void SessionManager::setInstanceUser(int index, const QString &username)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].username = username;
        Q_EMIT instancesChanged();
        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

void SessionManager::setInstanceMonitor(int index, int monitor)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].monitor = monitor;
        Q_EMIT instancesChanged();
        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

void SessionManager::setInstanceResolution(int index, int internalW, int internalH, int outputW, int outputH)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        InstanceConfig &inst = m_currentProfile.instances[index];
        inst.internalWidth = internalW;
        inst.internalHeight = internalH;
        inst.outputWidth = outputW;
        inst.outputHeight = outputH;
        Q_EMIT instancesChanged();
        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

void SessionManager::setInstanceDevices(int index, const QList<int> &devices)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].devices = devices;
        Q_EMIT instancesChanged();

        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

void SessionManager::setInstanceDeviceStableIds(int index, const QStringList &stableIds, const QStringList &names)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].deviceStableIds = stableIds;
        m_currentProfile.instances[index].deviceStableIdNames = names;
        Q_EMIT instancesChanged();

        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

void SessionManager::setInstanceGame(int index, const QString &gameCommand)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].gameCommand = gameCommand;
        Q_EMIT instancesChanged();

        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

void SessionManager::setInstancePreset(int index, const QString &presetId)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].presetId = presetId;
        Q_EMIT instancesChanged();

        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

void SessionManager::setInstanceDataDirectories(int index, const QVariantList &directories)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        QList<DataDirectory> dataDirs;
        for (const QVariant &var : directories) {
            DataDirectory dir = DataDirectory::fromVariant(var);
            if (!dir.path.isEmpty()) {
                dataDirs.append(dir);
            }
        }
        m_currentProfile.instances[index].dataDirectories = dataDirs;
        m_currentProfile.instances[index].dataDirectoriesSnapshotted = true;
        Q_EMIT instancesChanged();

        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

QString playerDataStagingRoot(const QString &presetId, const QString &username)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/player-data/")
        + presetId + QLatin1Char('/') + username;
}

QString SessionManager::playerDataFolderPath(int index)
{
    if (index < 0 || index >= m_currentProfile.instances.size()) {
        return QString();
    }

    const InstanceConfig &inst = m_currentProfile.instances[index];
    if (inst.username.isEmpty()) {
        return QString();
    }
    const QString presetId = inst.presetId.isEmpty() ? QStringLiteral("steam") : inst.presetId;

    const QString root = playerDataStagingRoot(presetId, inst.username);
    if (!QDir().mkpath(root)) {
        qWarning() << "SessionManager: Failed to create player data folder:" << root;
        Q_EMIT errorOccurred(QStringLiteral("Could not create player data folder"));
        return QString();
    }

    // One subfolder per private (copy/overlay) shared directory so users see
    // where files go; bind mounts are shared with everyone, so seeding them
    // per-player is impossible (writes would mutate the shared source).
    // Resolve the same effective list as SessionRunner's fallback: unsnapshotted
    // instances use the preset's current defaults.
    QList<DataDirectory> effectiveDirs = inst.dataDirectories;
    if (effectiveDirs.isEmpty() && !inst.dataDirectoriesSnapshotted && m_presetManager) {
        effectiveDirs = m_presetManager->getPreset(presetId).dataDirectories;
    }

    // The Steam-root overlay entry is a library-sharing marker handled
    // entirely by session setup (libraries are alias-mounted) — it never
    // receives staged data, so don't create a misleading folder for it.
    // Match the runtime predicate exactly (SessionRunner::setupDataDirectories):
    // the instance runs the Steam launcher, the entry is overlay-mode, and the
    // path is the detected Steam root. Anything else — the same path in copy
    // mode, or on another launcher's preset — is an ordinary private directory
    // that session setup stages data for, so it keeps its folder.
    QString steamMarkerPath;
    if (m_presetManager && m_presetManager->getPreset(presetId).launcherId == QStringLiteral("steam")) {
        steamMarkerPath = m_presetManager->getPreset(QStringLiteral("steam")).launcherInfo.configPath;
    }

    struct passwd *pw = getpwuid(getuid());
    QString compositorHome = pw ? QString::fromLocal8Bit(pw->pw_dir) : QString();
    for (const DataDirectory &dir : effectiveDirs) {
        if (dir.mode != QStringLiteral("copy") && dir.mode != QStringLiteral("overlay")) {
            continue;
        }
        if (!steamMarkerPath.isEmpty() && dir.path == steamMarkerPath
            && dir.mode == QStringLiteral("overlay")) {
            continue;
        }
        QDir().mkpath(root + QLatin1Char('/') + dataDirectoryStagingSlug(dir.path, compositorHome));
    }

    return root;
}

bool SessionManager::openPlayerDataFolder(int index)
{
    if (qEnvironmentVariableIsSet("FLATPAK_ID")) {
        // No host filesystem access from the sandbox; the caller should show
        // the path from playerDataFolderPath() instead
        return false;
    }

    const QString path = playerDataFolderPath(index);
    if (path.isEmpty()) {
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

QVariantList SessionManager::savedProfilesAsVariant() const
{
    QVariantList list;
    for (const auto &profile : m_savedProfiles) {
        QVariantMap map;
        map[QStringLiteral("name")] = profile.name;
        map[QStringLiteral("layout")] = profile.layout;
        map[QStringLiteral("filePath")] = profile.filePath;
        list.append(map);
    }
    return list;
}

QVariantList SessionManager::instancesAsVariant() const
{
    QVariantList list;
    for (int i = 0; i < m_currentProfile.instances.size(); ++i) {
        list.append(getInstanceConfig(i));
    }
    return list;
}

void SessionManager::recalculateOutputResolutions(int screenWidth, int screenHeight)
{
    int count = m_currentProfile.instances.size();
    if (count < 1) {
        return;
    }

    QString layout = m_currentProfile.layout;

    for (int i = 0; i < count; ++i) {
        InstanceConfig &inst = m_currentProfile.instances[i];

        if (layout == QStringLiteral("horizontal")) {
            inst.outputWidth = screenWidth / count;
            inst.outputHeight = screenHeight;
        } else if (layout == QStringLiteral("vertical")) {
            inst.outputWidth = screenWidth;
            inst.outputHeight = screenHeight / count;
        } else if (layout == QStringLiteral("grid")) {
            QList<QRect> layouts = SessionRunner::calculateLayout(layout,
                                                                  count,
                                                                  QRect(0, 0, screenWidth, screenHeight),
                                                                  m_currentProfile.gridSubLayout);
            if (i < layouts.size()) {
                inst.outputWidth = layouts[i].width();
                inst.outputHeight = layouts[i].height();
            }
        } else {
            // multi-monitor or unknown: use full resolution
            inst.outputWidth = screenWidth;
            inst.outputHeight = screenHeight;
        }

        inst.internalWidth = inst.outputWidth;
        inst.internalHeight = inst.outputHeight;
    }

    Q_EMIT instancesChanged();
}

QStringList SessionManager::getAssignedUsers(int excludeIndex) const
{
    QStringList assigned;
    for (int i = 0; i < m_currentProfile.instances.size(); ++i) {
        if (i != excludeIndex) {
            const QString &user = m_currentProfile.instances[i].username;
            if (!user.isEmpty() && !assigned.contains(user)) {
                assigned.append(user);
            }
        }
    }
    return assigned;
}
