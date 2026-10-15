// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "SessionManager.h"
#include "Logging.h"
#include "SessionRunner.h"
#include "UserLookup.h"
#include "../../helper/DirectoryPlan.h"

#include <QDesktopServices>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>
#include <QRegularExpression>

#include <fcntl.h>
#include <limits>
#include <unistd.h>


namespace {
QString gameSelectionToJson(const GameSelection &selection)
{
    return QString::fromUtf8(QJsonDocument::fromVariant(selection.toVariant()).toJson(QJsonDocument::Compact));
}

GameSelection gameSelectionFromJson(const QString &value)
{
    const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8());
    if (!document.isObject()) {
        return {};
    }
    return GameSelection::fromVariant(document.object().toVariantMap());
}


QString steamRootOverlayMarkerPath(const PresetManager *presetManager, const QString &presetId)
{
    if (!presetManager
        || !presetManager->getPreset(presetId).requiredIntegrations.contains(QStringLiteral("steam"))) {
        return QString();
    }
    return presetManager->getPreset(QStringLiteral("steam")).launcherInfo.configPath;
}

bool isPlayerDataStagingDirectory(const DataDirectory &directory, const QString &steamMarkerPath)
{
    if (directory.mode != QStringLiteral("copy") && directory.mode != QStringLiteral("overlay")) {
        return false;
    }
    return steamMarkerPath.isEmpty() || directory.mode != QStringLiteral("overlay")
        || directory.path != steamMarkerPath;
}

}

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

void SessionManager::setHelperClient(CouchPlayHelperClient *client)
{
    if (m_helperClient != client) {
        m_helperClient = client;
        Q_EMIT helperClientChanged();
    }
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

bool SessionManager::isValidProfileName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed != name || trimmed == QStringLiteral(".") || trimmed == QStringLiteral("..")) {
        return false;
    }
    for (const QChar character : trimmed) {
        if (character == QLatin1Char('/') || character == QLatin1Char('\\')
            || character.unicode() < 0x20 || character.unicode() == 0x7f) {
            return false;
        }
    }
    return true;
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
    Q_EMIT preSessionExecutableChanged();
    Q_EMIT postSessionExecutableChanged();
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
        if (!isValidProfileName(name)) {
            continue;
        }

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
    const QString profileName = name.trimmed();
    if (!isValidProfileName(profileName)) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid profile name"));
        return false;
    }

    const QString path = profilePath(profileName);
    if ((QFileInfo(path).exists() || QFileInfo(path).isSymLink()) && !migrateProfileDirectorySnapshots(path)) {
        return false;
    }
    KConfig config(path);

    KConfigGroup general = config.group(QStringLiteral("General"));
    general.writeEntry("name", profileName);
    general.writeEntry("layout", m_currentProfile.layout);
    general.writeEntry("gridSubLayout", m_currentProfile.gridSubLayout);
    general.writeEntry("preSessionExecutable", m_currentProfile.preSessionExecutable);
    general.writeEntry("postSessionExecutable", m_currentProfile.postSessionExecutable);
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
        if (inst.gameSelection.isEmpty()) {
            instGroup.deleteEntry("gameSelection");
        } else {
            instGroup.writeEntry("gameSelection", gameSelectionToJson(inst.gameSelection));
        }
        instGroup.writeEntry("presetId", inst.presetId);
        instGroup.writeEntry("privateTemporaryDirectories", inst.privateTemporaryDirectories);
        instGroup.deleteEntry("gameCommand");
        instGroup.deleteEntry("steamAppId");
        instGroup.deleteEntry("overrideGamePath");
        instGroup.deleteEntry("overrideFiles");
        instGroup.deleteEntry("overridePatterns");
        instGroup.deleteEntry("overlayGamePath");
        instGroup.deleteEntry("overlayPatterns");
        instGroup.writeEntry("outputMode", inst.outputMode);
        instGroup.writeEntry("streamResolution", inst.streamResolution);
        instGroup.writeEntry("streamFps", inst.streamFps);
        instGroup.writeEntry("streamBitrate", inst.streamBitrate);
        instGroup.writeEntry("streamCodec", inst.streamCodec);
        instGroup.writeEntry("sunshinePort", inst.sunshinePort);

        QStringList deviceStrings;
        for (int dev : inst.devices) {
            deviceStrings << QString::number(dev);
        }
        instGroup.writeEntry("devices", deviceStrings);
        instGroup.writeEntry("deviceStableIds", inst.deviceStableIds);
        instGroup.writeEntry("deviceStableIdNames", inst.deviceStableIdNames);
    }

    config.sync();

    m_currentProfile.name = profileName;
    m_currentProfile.filePath = path;

    Q_EMIT currentProfileChanged();
    refreshProfiles();
    return true;
}

bool SessionManager::migrateProfileDirectorySnapshots(const QString &path)
{
    const QFileInfo sourceInfo(path);
    if (sourceInfo.isSymLink() || !sourceInfo.isFile()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: unsafe profile file"));
        return false;
    }

    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: %1").arg(source.errorString()));
        return false;
    }
    const QByteArray original = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: %1").arg(source.errorString()));
        return false;
    }
    source.close();

    KConfig config(path, KConfig::SimpleConfig);
    QStringList legacyGroups;
    static const QRegularExpression instanceGroup(QStringLiteral("^Instance[0-9]+$"));
    for (const QString &name : config.groupList()) {
        if (!instanceGroup.match(name).hasMatch()) {
            continue;
        }
        const KConfigGroup group = config.group(name);
        if (group.hasKey(QStringLiteral("dataDirectories")) || group.hasKey(QStringLiteral("sharedDirectories"))) {
            legacyGroups.append(name);
        }
    }
    if (legacyGroups.isEmpty()) {
        return true;
    }

    QTemporaryFile temporary(QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral(".profile-migration-XXXXXX")));
    if (!temporary.open() || temporary.write(original) != original.size() || !temporary.flush()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: could not stage profile rewrite"));
        return false;
    }
    temporary.close();
    KConfig rewrittenConfig(temporary.fileName(), KConfig::SimpleConfig);
    for (const QString &name : legacyGroups) {
        KConfigGroup group = rewrittenConfig.group(name);
        group.deleteEntry(QStringLiteral("dataDirectories"));
        group.deleteEntry(QStringLiteral("sharedDirectories"));
    }
    if (!rewrittenConfig.sync()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: could not sync profile rewrite"));
        return false;
    }

    QFile rewritten(temporary.fileName());
    if (!rewritten.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: could not read rewritten profile"));
        return false;
    }
    const QByteArray migrated = rewritten.readAll();
    if (rewritten.error() != QFileDevice::NoError) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: could not read rewritten profile"));
        return false;
    }
    rewritten.close();

    bool backupReady = false;
    const QString backupBase = path + QStringLiteral(".pre-directory-snapshot.bak");
    for (int suffix = 1; suffix < std::numeric_limits<int>::max(); ++suffix) {
        const QString backupPath = suffix == 1 ? backupBase : backupBase + QStringLiteral(".%1").arg(suffix);
        const QFileInfo backupInfo(backupPath);
        if (backupInfo.exists() || backupInfo.isSymLink()) {
            if (backupInfo.isSymLink() || !backupInfo.isFile()) {
                Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: unsafe backup path"));
                return false;
            }
            QFile existingBackup(backupPath);
            if (!existingBackup.open(QIODevice::ReadOnly)) {
                Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: could not inspect backup"));
                return false;
            }
            const bool identical = existingBackup.readAll() == original
                && existingBackup.error() == QFileDevice::NoError;
            existingBackup.close();
            if (identical) {
                backupReady = true;
                break;
            }
            continue;
        }

        QFile backup(backupPath);
        if (!backup.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (QFileInfo(backupPath).exists() || QFileInfo(backupPath).isSymLink()) {
                continue;
            }
            Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: could not create backup"));
            return false;
        }
        const bool permissionsSet = backup.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const bool written = permissionsSet && backup.write(original) == original.size() && backup.flush()
            && ::fsync(backup.handle()) == 0;
        backup.close();
        if (!written || !permissionsSet) {
            QFile::remove(backupPath);
            Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: could not persist backup"));
            return false;
        }
        backupReady = true;
        break;
    }
    if (!backupReady) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: no backup name available"));
        return false;
    }

    const QByteArray directoryPath = QFile::encodeName(QFileInfo(path).absolutePath());
    const int directoryFd = ::open(directoryPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const bool directorySynced = directoryFd >= 0 && ::fsync(directoryFd) == 0;
    if (directoryFd >= 0) {
        ::close(directoryFd);
    }
    if (!directorySynced) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: could not persist backup directory"));
        return false;
    }

    QFile current(path);
    if (!current.open(QIODevice::ReadOnly) || current.readAll() != original
        || current.error() != QFileDevice::NoError || QFileInfo(path).isSymLink()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: source changed during migration"));
        return false;
    }
    current.close();

    QSaveFile destination(path);
    if (!destination.open(QIODevice::WriteOnly) || destination.write(migrated) != migrated.size()
        || !destination.commit()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to migrate profile: atomic replacement failed"));
        return false;
    }
    return true;
}
bool SessionManager::loadProfile(const QString &name)
{
    const QString profileName = name.trimmed();
    if (!isValidProfileName(profileName)) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid profile name"));
        return false;
    }
    QString path = profilePath(profileName);

    if (!QFile::exists(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Profile not found: %1").arg(name));
        return false;
    }
    if (!migrateProfileDirectorySnapshots(path)) {
        return false;
    }

    KConfig config(path);

    KConfigGroup general = config.group(QStringLiteral("General"));
    m_currentProfile.name = profileName;
    m_currentProfile.filePath = path;
    m_currentProfile.layout = general.readEntry("layout", QStringLiteral("horizontal"));
    m_currentProfile.gridSubLayout = general.readEntry("gridSubLayout", QString());
    m_currentProfile.preSessionExecutable = general.readEntry("preSessionExecutable", QString());
    m_currentProfile.postSessionExecutable = general.readEntry("postSessionExecutable", QString());
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
        inst.presetId = instGroup.readEntry("presetId", QStringLiteral("steam"));
        inst.privateTemporaryDirectories = instGroup.readEntry("privateTemporaryDirectories", false);
        if (instGroup.hasKey("gameSelection")) {
            inst.gameSelection = gameSelectionFromJson(instGroup.readEntry("gameSelection", QString()));
        } else {
            const QString legacySteamAppId = instGroup.readEntry("steamAppId", QString());
            if (inst.presetId == QStringLiteral("steam") && !legacySteamAppId.isEmpty()) {
                inst.gameSelection.launcherId = QStringLiteral("steam");
                inst.gameSelection.backend = QStringLiteral("native");
                inst.gameSelection.gameId = legacySteamAppId;
            }
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
    Q_EMIT preSessionExecutableChanged();
    Q_EMIT postSessionExecutableChanged();
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
    const QString profileName = name.trimmed();
    if (!isValidProfileName(profileName)) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid profile name"));
        return false;
    }
    QString path = profilePath(profileName);

    if (!QFile::exists(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Profile not found: %1").arg(name));
        return false;
    }

    if (!QFile::remove(path)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to delete profile: %1").arg(name));
        return false;
    }

    if (m_currentProfile.name == profileName) {
        m_currentProfile.name = QString();
        m_currentProfile.filePath = QString();
        Q_EMIT currentProfileChanged();
    }

    refreshProfiles();
    return true;
}

QString SessionManager::duplicateProfile(const QString &sourceName)
{
    const QString sourceNameTrimmed = sourceName.trimmed();
    if (!isValidProfileName(sourceNameTrimmed)) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid profile name"));
        return {};
    }

    const QString sourcePath = profilePath(sourceNameTrimmed);
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile()) {
        Q_EMIT errorOccurred(QStringLiteral("Profile not found: %1").arg(sourceNameTrimmed));
        return {};
    }
    if (!migrateProfileDirectorySnapshots(sourcePath)) {
        return {};
    }

    QString destinationName = sourceNameTrimmed + QStringLiteral(" Copy");
    int copyNumber = 2;
    while (QFile::exists(profilePath(destinationName))) {
        destinationName = sourceNameTrimmed + QStringLiteral(" Copy %1").arg(copyNumber++);
    }

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to read profile: %1").arg(sourceNameTrimmed));
        return {};
    }
    const QByteArray contents = source.readAll();
    source.close();

    QTemporaryFile temporary(QDir(profilesDir()).filePath(QStringLiteral(".profile-copy-XXXXXX")));
    temporary.setAutoRemove(true);
    if (!temporary.open() || temporary.write(contents) != contents.size()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to prepare profile copy"));
        return {};
    }
    temporary.close();

    KConfig copiedConfig(temporary.fileName(), KConfig::SimpleConfig);
    copiedConfig.group(QStringLiteral("General")).writeEntry("name", destinationName);
    copiedConfig.sync();

    QFile rewritten(temporary.fileName());
    if (!rewritten.open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to finalize profile copy"));
        return {};
    }
    const QByteArray rewrittenContents = rewritten.readAll();
    rewritten.close();

    QSaveFile destination(profilePath(destinationName));
    if (!destination.open(QIODevice::WriteOnly) || destination.write(rewrittenContents) != rewrittenContents.size()
        || !destination.commit()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to write profile copy: %1").arg(destinationName));
        return {};
    }

    refreshProfiles();
    return destinationName;
}


void SessionManager::setPreSessionExecutable(const QString &path)
{
    const QString normalized = path;
    if (m_currentProfile.preSessionExecutable == normalized) {
        return;
    }
    m_currentProfile.preSessionExecutable = normalized;
    Q_EMIT preSessionExecutableChanged();
    if (!m_currentProfile.name.isEmpty()) {
        saveProfile(m_currentProfile.name);
    }
}

void SessionManager::setPostSessionExecutable(const QString &path)
{
    const QString normalized = path;
    if (m_currentProfile.postSessionExecutable == normalized) {
        return;
    }
    m_currentProfile.postSessionExecutable = normalized;
    Q_EMIT postSessionExecutableChanged();
    if (!m_currentProfile.name.isEmpty()) {
        saveProfile(m_currentProfile.name);
    }
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
    map[QStringLiteral("gameSelection")] = inst.gameSelectionAsVariant();
    map[QStringLiteral("presetId")] = inst.presetId;
    map[QStringLiteral("privateTemporaryDirectories")] = inst.privateTemporaryDirectories;
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
    if (config.contains(QStringLiteral("gameSelection"))) {
        inst.gameSelection = GameSelection::fromVariant(config.value(QStringLiteral("gameSelection")).toMap());
    }
    if (config.contains(QStringLiteral("presetId")))
        inst.presetId = config[QStringLiteral("presetId")].toString();
    if (config.contains(QStringLiteral("privateTemporaryDirectories")))
        inst.privateTemporaryDirectories = config[QStringLiteral("privateTemporaryDirectories")].toBool();
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

void SessionManager::setInstanceGame(int index, const QVariantMap &selection)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        m_currentProfile.instances[index].gameSelection = GameSelection::fromVariant(selection);
        Q_EMIT instancesChanged();

        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}

void SessionManager::setInstancePreset(int index, const QString &presetId)
{
    if (index >= 0 && index < m_currentProfile.instances.size()) {
        const QString normalizedPresetId = presetId.isEmpty() ? QStringLiteral("steam") : presetId;
        m_currentProfile.instances[index].presetId = normalizedPresetId;
        if (!m_currentProfile.instances[index].gameSelection.isEmpty() && m_presetManager
            && m_currentProfile.instances[index].gameSelection.launcherId != m_presetManager->getLauncherId(normalizedPresetId)) {
            m_currentProfile.instances[index].gameSelection = {};
        }
        Q_EMIT instancesChanged();

        if (!m_currentProfile.name.isEmpty()) {
            saveProfile(m_currentProfile.name);
        }
    }
}


QString playerDataStagingRoot(const QString &presetId, const QString &username)
{
    // Native installs use the conventional CouchPlay data directory. Flatpak
    // exports XDG_DATA_HOME to its host-visible per-app data directory, so
    // using AppDataLocation here gives the GUI and the root helper the same
    // absolute path and inode.
    QString dataRoot;
    if (qEnvironmentVariableIsSet("FLATPAK_ID")) {
        dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    } else {
        dataRoot = QDir::homePath() + QStringLiteral("/.local/share/couchplay");
    }
    return dataRoot + QStringLiteral("/player-data/") + presetId + QLatin1Char('/') + username;
}

bool SessionManager::hasPlayerDataStagingDirectories(int index) const
{
    if (index < 0 || index >= m_currentProfile.instances.size()) {
        return false;
    }

    const InstanceConfig &inst = m_currentProfile.instances[index];
    if (inst.username.isEmpty()) {
        return false;
    }
    const QString presetId = inst.presetId.isEmpty() ? QStringLiteral("steam") : inst.presetId;
    const LaunchPreset preset = m_presetManager ? m_presetManager->getPreset(presetId) : LaunchPreset{};
    const QString steamMarkerPath = steamRootOverlayMarkerPath(m_presetManager, presetId);
    for (const DataDirectory &dir : preset.dataDirectories) {
        if (isPlayerDataStagingDirectory(dir, steamMarkerPath)) {
            return true;
        }
    }
    return false;
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

    // One subfolder per private (copy/overlay) shared directory. Bind mounts
    // are shared with everyone, so seeding them per-player would mutate the
    // shared source.
    const LaunchPreset preset = m_presetManager ? m_presetManager->getPreset(presetId) : LaunchPreset{};
    const QString steamMarkerPath = steamRootOverlayMarkerPath(m_presetManager, presetId);

    // Helper-first so the slug matches SessionRunner's target mapping under
    // Flatpak, where the sandbox resolves the compositor's home differently
    const QString compositorHome = resolveCompositorHome(m_helperClient);
    for (const DataDirectory &dir : preset.dataDirectories) {
        if (!isPlayerDataStagingDirectory(dir, steamMarkerPath)) {
            continue;
        }
        QDir().mkpath(root + QLatin1Char('/') + DirectoryPlan::dataDirectoryStagingSlug(dir.path, compositorHome));
    }

    return root;
}

bool SessionManager::openPlayerDataFolder(int index)
{
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
