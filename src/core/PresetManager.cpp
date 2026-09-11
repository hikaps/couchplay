// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "PresetManager.h"
#include "CommandVerifier.h"
#include "HeroicConfigManager.h"
#include "Logging.h"
#include "SteamConfigManager.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

#include <functional>

#include <KSharedConfig>
#include <KConfigGroup>

static constexpr int CACHE_TTL_HOURS = 24;
static constexpr int FLATPAK_CACHE_VERSION = 2;  // bump when builtin preset commands change

PresetManager::PresetManager(QObject *parent)
    : QObject(parent)
{
    initBuiltinPresets();
    loadCustomPresets();
}

PresetManager::~PresetManager() = default;

void PresetManager::setHeroicConfigManager(HeroicConfigManager *manager)
{
    if (m_heroicConfigManager != manager) {
        m_heroicConfigManager = manager;
        initBuiltinPresets();
        Q_EMIT presetsChanged();
    }
}

void PresetManager::setSteamConfigManager(SteamConfigManager *manager)
{
    if (m_steamConfigManager != manager) {
        m_steamConfigManager = manager;
        initBuiltinPresets();
        Q_EMIT presetsChanged();
    }
}

DataDirectory DataDirectory::fromVariant(const QVariant &var)
{
    DataDirectory dir;
    if (var.canConvert<DataDirectory>()) {
        dir = var.value<DataDirectory>();
    } else if (var.userType() == QMetaType::QVariantMap) {
        const QVariantMap map = var.toMap();
        dir.path = map[QStringLiteral("path")].toString();
        dir.mode = map[QStringLiteral("mode")].toString();
    }
    if (dir.mode != QStringLiteral("acl") && dir.mode != QStringLiteral("copy") && dir.mode != QStringLiteral("overlay")
        && dir.mode != QStringLiteral("bind")) {
        dir.mode = QStringLiteral("acl");
    }
    return dir;
}

QString dataDirectoryStagingSlug(const QString &dirPath, const QString &compositorHome)
{
    QString relative;
    if (!compositorHome.isEmpty() && dirPath.startsWith(compositorHome + QLatin1Char('/'))) {
        relative = dirPath.mid(compositorHome.length() + 1);
    } else {
        // External path: sanitize the full path into a unique folder name
        relative = dirPath;
        while (relative.startsWith(QLatin1Char('/'))) {
            relative.remove(0, 1);
        }
    }
    QString slug = relative;
    slug.replace(QLatin1Char('/'), QLatin1Char('_'));
    // "_"-collapsing is not injective (/a_b/c vs /a/b_c); append a short
    // stable hash of the normalized path so distinct sources can't share
    // a staging folder and bleed data into each other's player views
    const QByteArray hash =
        QCryptographicHash::hash(relative.toUtf8(), QCryptographicHash::Sha256).toHex().left(8);
    slug += QLatin1Char('-') + QString::fromLatin1(hash);
    return slug;
}

QString encodeDataDirectories(const QList<DataDirectory> &directories)
{
    QJsonArray array;
    for (const DataDirectory &dir : directories) {
        QJsonObject entry;
        entry.insert(QStringLiteral("path"), dir.path);
        entry.insert(QStringLiteral("mode"), dir.mode);
        array.append(entry);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QList<DataDirectory> decodeDataDirectories(const QString &payload)
{
    QList<DataDirectory> dirs;
    const QString trimmed = payload.trimmed();

    // New format: JSON array. Paths containing '|' or newlines cannot survive
    // the legacy line format, so anything that parses as JSON wins.
    if (trimmed.startsWith(QLatin1Char('['))) {
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8());
        if (doc.isArray()) {
            const QJsonArray array = doc.array();
            for (const QJsonValue &value : array) {
                const QJsonObject entry = value.toObject();
                QVariantMap map;
                map[QStringLiteral("path")] = entry.value(QStringLiteral("path")).toString();
                map[QStringLiteral("mode")] = entry.value(QStringLiteral("mode")).toString();
                // fromVariant skips nothing but sanitizes the mode; drop
                // entries without a path, mirroring the legacy parser
                const DataDirectory dir = DataDirectory::fromVariant(map);
                if (!dir.path.isEmpty()) {
                    dirs.append(dir);
                }
            }
            return dirs;
        }
        // Malformed JSON: fall through to the legacy parser, which yields
        // nothing usable for a '['-prefixed line — fail closed to empty
        return dirs;
    }

    // Legacy format: newline-separated "path|mode" entries (written by
    // earlier versions; kept so existing presets and profiles load)
    const QStringList entries = trimmed.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const int pipePos = entry.indexOf(QLatin1Char('|'));
        if (pipePos > 0) {
            QVariantMap map;
            map[QStringLiteral("path")] = entry.left(pipePos);
            map[QStringLiteral("mode")] = entry.mid(pipePos + 1);
            dirs.append(DataDirectory::fromVariant(map));
        }
    }
    return dirs;
}

QList<DataDirectory> PresetManager::getDefaultDataDirectories(const QString &id) const
{
    using Resolver = std::function<QList<DataDirectory>(const PresetManager *)>;
    static const QHash<QString, Resolver> resolvers = {
        {QStringLiteral("steam"), [](const PresetManager *self) -> QList<DataDirectory> {
            QList<DataDirectory> dirs;
            if (!self->m_steamConfigManager || !self->m_steamConfigManager->isSteamDetected()) {
                return dirs;
            }

            // Only the library-sharing overlay comes from the preset; shortcut
            // directories are computed and ACLed live at session start by
            // SessionRunner (gated on syncShortcutsEnabled, which may change
            // after this snapshot is taken)
            QString steamRoot = self->m_steamConfigManager->steamPaths().steamRoot;
            if (!steamRoot.isEmpty()) {
                dirs.append({steamRoot, QStringLiteral("overlay")});
            }

            return dirs;
        }},
        {QStringLiteral("heroic"), [](const PresetManager *self) -> QList<DataDirectory> {
            QList<DataDirectory> dirs;
            if (!self->m_heroicConfigManager || !self->m_heroicConfigManager->isHeroicDetected()) {
                return dirs;
            }

            // Config sync (syncConfigToUser) is dispatched at session start by
            // SessionRunner; only game data comes from the preset snapshot
            QString installPath = self->m_heroicConfigManager->defaultInstallPath();
            if (!installPath.isEmpty()) {
                dirs.append({installPath, QStringLiteral("overlay")});
            }

            if (self->m_heroicConfigManager->gameCount() == 0) {
                self->m_heroicConfigManager->loadGames();
            }
            QStringList gameDirs = self->m_heroicConfigManager->extractGameDirectories();
            for (const QString &dir : gameDirs) {
                if (QDir(dir).exists()) {
                    dirs.append({dir, QStringLiteral("acl")});
                }
            }

            return dirs;
        }},
        {QStringLiteral("lutris"), [](const PresetManager *) -> QList<DataDirectory> {
            QList<DataDirectory> dirs;
            QString home = QDir::homePath();
            QString lutrisData = home + QStringLiteral("/.local/share/lutris");
            QString lutrisGames = home + QStringLiteral("/Games");
            if (QDir(lutrisData).exists()) {
                dirs.append({lutrisData, QStringLiteral("acl")});
            }
            if (QDir(lutrisGames).exists()) {
                dirs.append({lutrisGames, QStringLiteral("acl")});
            }
            return dirs;
        }},
    };

    QList<DataDirectory> dirs = resolvers.value(id, [](const PresetManager *) -> QList<DataDirectory> { return {}; })(this);

    // Deduplicate by path
    QStringList seen;
    QList<DataDirectory> unique;
    for (const DataDirectory &d : dirs) {
        if (!seen.contains(d.path)) {
            seen.append(d.path);
            unique.append(d);
        }
    }
    return unique;
}

QString PresetManager::resolveLaunchCommand(const QString &nativeCommand,
                                              const QString &flatpakAppId,
                                              const QString &flatpakArgs) const
{
    QString binaryName = nativeCommand.split(QStringLiteral(" ")).first();

    if (CommandVerifier::commandExistsInPath(binaryName)) {
        return nativeCommand;
    }

    if (!flatpakAppId.isEmpty()
        && CommandVerifier::isFlatpakAvailable()
        && CommandVerifier::isFlatpakAppInstalled(flatpakAppId)) {
        QString cmd = QStringLiteral("flatpak run ") + flatpakAppId;
        if (!flatpakArgs.isEmpty()) {
            cmd += QStringLiteral(" ") + flatpakArgs;
        }
        return cmd;
    }

    return nativeCommand;
}

void PresetManager::loadFlatpakCache()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup cacheGroup = config->group(QStringLiteral("FlatpakCache"));
    if (cacheGroup.readEntry(QStringLiteral("version"), 0) != FLATPAK_CACHE_VERSION) {
        return;  // stale cache (builtin commands changed) - saveFlatpakCache rewrites it
    }

    const QStringList keys = cacheGroup.keyList();
    QSet<QString> presetIds;
    for (const QString &key : keys) {
        int slash = key.indexOf(QStringLiteral("/"));
        if (slash > 0) {
            presetIds.insert(key.left(slash));
        }
    }

    QDateTime now = QDateTime::currentDateTime();
    for (const QString &presetId : presetIds) {
        QString timestampStr = cacheGroup.readEntry(presetId + QStringLiteral("/timestamp"), QString());
        if (timestampStr.isEmpty()) {
            continue;
        }
        QDateTime cachedTime = QDateTime::fromString(timestampStr, QStringLiteral("yyyyMMddTHHmmss"));
        if (!cachedTime.isValid() || cachedTime.secsTo(now) > CACHE_TTL_HOURS * 3600) {
            continue;
        }
        QString cachedCmd = cacheGroup.readEntry(presetId + QStringLiteral("/command"), QString());
        if (!cachedCmd.isEmpty()) {
            for (auto &preset : m_builtinPresets) {
                if (preset.id == presetId) {
                    preset.command = cachedCmd;
                    break;
                }
            }
        }
    }
}

void PresetManager::saveFlatpakCache()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup cacheGroup = config->group(QStringLiteral("FlatpakCache"));
    cacheGroup.writeEntry(QStringLiteral("version"), FLATPAK_CACHE_VERSION);

    QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddTHHmmss"));
    for (const auto &preset : m_builtinPresets) {
        if (!preset.flatpakAppId.isEmpty()) {
            cacheGroup.writeEntry(preset.id + QStringLiteral("/command"), preset.command);
            cacheGroup.writeEntry(preset.id + QStringLiteral("/timestamp"), timestamp);
        }
    }
    cacheGroup.sync();
}

QString PresetManager::defaultSteamCommand()
{
    return QStringLiteral("steam -bigpicture");
}
void PresetManager::initBuiltinPresets()
{
    m_builtinPresets.clear();

    LaunchPreset steam;
    steam.id = QStringLiteral("steam");
    steam.name = QStringLiteral("Steam Big Picture");
    steam.flatpakAppId = QStringLiteral("com.valvesoftware.Steam");
    steam.flatpakArgs = QStringLiteral("-bigpicture");
    steam.command = resolveLaunchCommand(
        defaultSteamCommand(),
        steam.flatpakAppId,
        steam.flatpakArgs);
    steam.iconName = QStringLiteral("steam");
    steam.isBuiltin = true;
    steam.launcherId = QStringLiteral("steam");

    if (m_steamConfigManager && m_steamConfigManager->isSteamDetected()) {
        steam.launcherInfo.configPath = m_steamConfigManager->steamPaths().steamRoot;
    }

    steam.dataDirectories = getDefaultDataDirectories(QStringLiteral("steam"));
    m_builtinPresets.append(steam);

    LaunchPreset heroic;
    heroic.id = QStringLiteral("heroic");
    heroic.name = QStringLiteral("Heroic Games");
    heroic.iconName = QStringLiteral("com.heroicgameslauncher.hgl");
    heroic.isBuiltin = true;
    heroic.launcherId = QStringLiteral("heroic");
    heroic.flatpakAppId = QStringLiteral("com.heroicgameslauncher.hgl");

    if (m_heroicConfigManager && m_heroicConfigManager->isHeroicDetected()) {
        heroic.command = resolveLaunchCommand(
            QStringLiteral("heroic"),
            heroic.flatpakAppId,
            QString());
        heroic.launcherInfo.configPath = m_heroicConfigManager->configPath();
        heroic.launcherInfo.dataPath = m_heroicConfigManager->defaultInstallPath();
    } else {
        heroic.command = resolveLaunchCommand(
            QStringLiteral("heroic"),
            heroic.flatpakAppId,
            QString());
    }

    heroic.dataDirectories = getDefaultDataDirectories(QStringLiteral("heroic"));
    m_builtinPresets.append(heroic);

    LaunchPreset lutris;
    lutris.id = QStringLiteral("lutris");
    lutris.name = QStringLiteral("Lutris");
    lutris.flatpakAppId = QStringLiteral("net.lutris.Lutris");
    lutris.command = resolveLaunchCommand(
        QStringLiteral("lutris"),
        lutris.flatpakAppId,
        QString());
    lutris.iconName = QStringLiteral("lutris");
    lutris.isBuiltin = true;
    lutris.launcherId = QStringLiteral("lutris");
    lutris.dataDirectories = getDefaultDataDirectories(QStringLiteral("lutris"));
    m_builtinPresets.append(lutris);

    loadFlatpakCache();
    saveFlatpakCache();

    // initBuiltinPresets re-runs whenever a config manager is (re)injected,
    // re-resolving detected defaults and wiping in-memory edits — reapply the
    // persisted user overrides so built-in preset edits survive restarts
    applyBuiltinDataDirectoryOverrides();
}

QList<LaunchPreset> PresetManager::presets() const
{
    QList<LaunchPreset> all = m_builtinPresets;
    all.append(m_customPresets);
    return all;
}

QVariantList PresetManager::presetsAsVariant() const
{
    QVariantList result;
    for (const LaunchPreset &preset : presets()) {
        QVariantMap map;
        map[QStringLiteral("id")] = preset.id;
        map[QStringLiteral("name")] = preset.name;
        map[QStringLiteral("command")] = preset.command;
        map[QStringLiteral("workingDirectory")] = preset.workingDirectory;
        map[QStringLiteral("iconName")] = preset.iconName;
        map[QStringLiteral("desktopFilePath")] = preset.desktopFilePath;
        map[QStringLiteral("isBuiltin")] = preset.isBuiltin;
        map[QStringLiteral("launcherId")] = preset.launcherId;
        map[QStringLiteral("launcherInfo")] = QVariant::fromValue(preset.launcherInfo);

        QVariantList dataDirsVariant;
        for (const DataDirectory &dir : preset.dataDirectories) {
            dataDirsVariant.append(QVariant::fromValue(dir));
        }
        map[QStringLiteral("dataDirectories")] = dataDirsVariant;

        map[QStringLiteral("flatpakAppId")] = preset.flatpakAppId;
        map[QStringLiteral("flatpakArgs")] = preset.flatpakArgs;
        result.append(map);
    }
    return result;
}

QVariantList PresetManager::availableApplicationsAsVariant() const
{
    QVariantList result;
    for (const LaunchPreset &app : m_availableApplications) {
        QVariantMap map;
        map[QStringLiteral("id")] = app.id;
        map[QStringLiteral("name")] = app.name;
        map[QStringLiteral("command")] = app.command;
        map[QStringLiteral("workingDirectory")] = app.workingDirectory;
        map[QStringLiteral("iconName")] = app.iconName;
        map[QStringLiteral("desktopFilePath")] = app.desktopFilePath;
        map[QStringLiteral("isBuiltin")] = app.isBuiltin;
        map[QStringLiteral("launcherId")] = app.launcherId;
        map[QStringLiteral("launcherInfo")] = QVariant::fromValue(app.launcherInfo);
        map[QStringLiteral("flatpakAppId")] = app.flatpakAppId;
        map[QStringLiteral("flatpakArgs")] = app.flatpakArgs;
        result.append(map);
    }
    return result;
}

void PresetManager::populateLauncherInfo(LaunchPreset &preset) const
{
    preset.launcherInfo = LauncherInfo();

    if (preset.launcherId == QStringLiteral("steam")) {
        if (m_steamConfigManager && m_steamConfigManager->isSteamDetected()) {
            preset.launcherInfo.configPath = m_steamConfigManager->steamPaths().steamRoot;
        }
    } else if (preset.launcherId == QStringLiteral("heroic")) {
        if (m_heroicConfigManager && m_heroicConfigManager->isHeroicDetected()) {
            preset.launcherInfo.configPath = m_heroicConfigManager->configPath();
            preset.launcherInfo.dataPath = m_heroicConfigManager->defaultInstallPath();
        }
    }
}

LaunchPreset PresetManager::getPreset(const QString &id) const
{
    for (const LaunchPreset &preset : m_builtinPresets) {
        if (preset.id == id) {
            return preset;
        }
    }
    for (const LaunchPreset &preset : m_customPresets) {
        if (preset.id == id) {
            LaunchPreset copy = preset;
            populateLauncherInfo(copy);
            return copy;
        }
    }
    if (!m_builtinPresets.isEmpty()) {
        return m_builtinPresets.first();
    }
    return LaunchPreset();
}

QString PresetManager::getCommand(const QString &id) const
{
    return getPreset(id).command;
}

QString PresetManager::getWorkingDirectory(const QString &id) const
{
    return getPreset(id).workingDirectory;
}

QString PresetManager::getLauncherId(const QString &id) const
{
    return getPreset(id).launcherId;
}

QVariantList PresetManager::getDataDirectories(const QString &id) const
{
    QVariantList result;
    const QList<DataDirectory> dirs = getPreset(id).dataDirectories;
    for (const DataDirectory &dir : dirs) {
        result.append(QVariant::fromValue(dir));
    }
    return result;
}
QString PresetManager::resolveDirectoryPath(const QString &path) const
{
    if (!qEnvironmentVariableIsSet("FLATPAK_ID")) {
        return path;
    }

    // The file chooser exposes granted documents through either the
    // sandbox-visible /run/flatpak/doc mount or the host-style
    // /run/user/<uid>/doc mount. Resolve only those exact namespaces; an
    // arbitrary path containing "/doc/" must never be treated as a portal
    // document.
    const QString cleanPath = QDir::cleanPath(path);
    static const QRegularExpression portalPathPattern(
        QStringLiteral("^/run/(?:flatpak/doc|user/[0-9]+/doc)/([^/]+)(/.*)?$"));
    const QRegularExpressionMatch match = portalPathPattern.match(cleanPath);
    if (!match.hasMatch()) {
        return path;
    }
    const QString documentId = match.captured(1);

    // GetHostPaths returns the host path for the exported document root. The
    // first suffix component is that same exported root name, so only append
    // components below it (if the chooser returned a descendant).
    const QString portalSuffix = match.captured(2);
    QString suffix;
    if (!portalSuffix.isEmpty()) {
        const int descendantSlash = portalSuffix.indexOf(QLatin1Char('/'), 1);
        if (descendantSlash >= 0) {
            suffix = portalSuffix.mid(descendantSlash);
        }
    }
    QDBusMessage request =
        QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.portal.Documents"),
                                       QStringLiteral("/org/freedesktop/portal/documents"),
                                       QStringLiteral("org.freedesktop.portal.Documents"),
                                       QStringLiteral("GetHostPaths"));
    request << QStringList{documentId};

    const QDBusMessage reply = QDBusConnection::sessionBus().call(request);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        qWarning() << "PresetManager: Failed to resolve document portal path" << path << ":"
                   << reply.errorMessage();
        return {};
    }

    const QDBusArgument paths = reply.arguments().constFirst().value<QDBusArgument>();
    QString hostPath;
    QDBusArgument map = paths;
    map.beginMap();
    while (!map.atEnd()) {
        QString id;
        QByteArray pathBytes;
        map.beginMapEntry();
        map >> id >> pathBytes;
        map.endMapEntry();
        if (id == documentId) {
            hostPath = QString::fromUtf8(pathBytes);
            break;
        }
    }
    map.endMap();

    if (hostPath.isEmpty() || !hostPath.startsWith(QLatin1Char('/'))) {
        qWarning() << "PresetManager: Documents portal returned no absolute host path for" << documentId;
        return {};
    }
    return QDir::cleanPath(hostPath + suffix);
}


bool PresetManager::setDataDirectories(const QString &id, const QVariantList &directories)
{
    QList<DataDirectory> dataDirs;
    for (const QVariant &var : directories) {
        DataDirectory dir = DataDirectory::fromVariant(var);
        if (!dir.path.isEmpty()) {
            dataDirs.append(dir);
        }
    }

    for (int i = 0; i < m_builtinPresets.size(); ++i) {
        if (m_builtinPresets[i].id == id) {
            m_builtinPresets[i].dataDirectories = dataDirs;
            saveBuiltinDataDirectoryOverride(id);
            Q_EMIT presetsChanged();
            return true;
        }
    }

    for (int i = 0; i < m_customPresets.size(); ++i) {
        if (m_customPresets[i].id == id) {
            m_customPresets[i].dataDirectories = dataDirs;
            saveCustomPresets();
            Q_EMIT presetsChanged();
            return true;
        }
    }

    qWarning() << "Cannot set data directories - preset not found:" << id;
    return false;
}

QString PresetManager::addCustomPreset(const QString &name,
                                        const QString &command,
                                        const QString &workingDirectory,
                                        const QString &iconName)
{
    LaunchPreset preset;
    preset.id = generateCustomId();
    preset.name = name;
    preset.command = command;
    preset.workingDirectory = workingDirectory;
    preset.iconName = iconName.isEmpty() ? QStringLiteral("application-x-executable") : iconName;
    preset.isBuiltin = false;

    m_customPresets.append(preset);
    saveCustomPresets();
    Q_EMIT presetsChanged();

    return preset.id;
}

QString PresetManager::addPresetFromDesktopFile(const QString &desktopFilePath)
{
    LaunchPreset preset = parseDesktopFile(desktopFilePath);
    if (preset.name.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to parse desktop file: %1").arg(desktopFilePath));
        return QString();
    }

    preset.launcherId = detectLauncherId(preset.command);

    for (const LaunchPreset &existing : m_customPresets) {
        if (existing.desktopFilePath == desktopFilePath) {
            return existing.id;
        }
    }

    preset.id = generateCustomId();
    preset.isBuiltin = false;

    m_customPresets.append(preset);
    saveCustomPresets();
    Q_EMIT presetsChanged();

    return preset.id;
}

bool PresetManager::updateCustomPreset(const QString &id,
                                        const QString &name,
                                        const QString &command,
                                        const QString &workingDirectory,
                                        const QString &iconName)
{
    for (int i = 0; i < m_customPresets.size(); ++i) {
        if (m_customPresets[i].id == id) {
            m_customPresets[i].name = name;
            m_customPresets[i].command = command;
            m_customPresets[i].workingDirectory = workingDirectory;
            m_customPresets[i].iconName = iconName;

            saveCustomPresets();
            Q_EMIT presetsChanged();

            return true;
        }
    }

    qWarning() << "Cannot update preset - not found or builtin:" << id;
    return false;
}

bool PresetManager::removeCustomPreset(const QString &id)
{
    for (int i = 0; i < m_customPresets.size(); ++i) {
        if (m_customPresets[i].id == id) {
            m_customPresets.removeAt(i);
            saveCustomPresets();
            Q_EMIT presetsChanged();

            return true;
        }
    }

    qWarning() << "Cannot remove preset - not found or builtin:" << id;
    return false;
}

void PresetManager::scanApplications()
{
    m_availableApplications.clear();

    QStringList searchPaths = {
        QStringLiteral("/usr/share/applications"),
        QStringLiteral("/usr/local/share/applications"),
        QDir::homePath() + QStringLiteral("/.local/share/applications"),
        QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/applications"),
        QStringLiteral("/var/lib/flatpak/exports/share/applications"),
        QStringLiteral("/var/lib/snapd/desktop/applications")
    };

    QSet<QString> seenNames;

    for (const QString &searchPath : searchPaths) {
        QDir dir(searchPath);
        if (!dir.exists()) {
            continue;
        }

        const QStringList desktopFiles = dir.entryList({QStringLiteral("*.desktop")}, QDir::Files);
        for (const QString &fileName : desktopFiles) {
            QString filePath = dir.absoluteFilePath(fileName);
            LaunchPreset app = parseDesktopFile(filePath);

            if (app.name.isEmpty() || seenNames.contains(app.name)) {
                continue;
            }

            bool alreadyAdded = false;
            for (const LaunchPreset &custom : m_customPresets) {
                if (custom.desktopFilePath == filePath) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (alreadyAdded) {
                continue;
            }

            seenNames.insert(app.name);
            m_availableApplications.append(app);
        }
    }

    std::sort(m_availableApplications.begin(), m_availableApplications.end(),
              [](const LaunchPreset &a, const LaunchPreset &b) {
                  return a.name.toLower() < b.name.toLower();
              });

    Q_EMIT applicationsChanged();
}

void PresetManager::refresh()
{
    loadCustomPresets();
    Q_EMIT presetsChanged();
}

LaunchPreset PresetManager::parseDesktopFile(const QString &filePath) const
{
    LaunchPreset preset;

    if (!QFile::exists(filePath)) {
        return preset;
    }

    QSettings desktop(filePath, QSettings::IniFormat);
    desktop.beginGroup(QStringLiteral("Desktop Entry"));

    QString type = desktop.value(QStringLiteral("Type")).toString();
    if (type != QStringLiteral("Application")) {
        return preset;
    }

    if (desktop.value(QStringLiteral("Hidden"), false).toBool() ||
        desktop.value(QStringLiteral("NoDisplay"), false).toBool()) {
        return preset;
    }

    preset.name = desktop.value(QStringLiteral("Name")).toString();
    preset.command = cleanExecCommand(desktop.value(QStringLiteral("Exec")).toString());
    preset.workingDirectory = desktop.value(QStringLiteral("Path")).toString();
    preset.iconName = desktop.value(QStringLiteral("Icon")).toString();
    preset.desktopFilePath = filePath;
    preset.launcherId = detectLauncherId(preset.command);

    // QString categories = desktop.value(QStringLiteral("Categories")).toString();

    return preset;
}

QString PresetManager::detectLauncherId(const QString &command) const
{
    if (command.isEmpty()) {
        return QString();
    }

    const QStringList tokens = command.split(QStringLiteral(" "), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        return QString();
    }

    const QString &firstToken = tokens.first();

    if (firstToken.contains(QStringLiteral("://"))) {
        return QString();
    }

    // Exported desktop files often carry the absolute executable
    // (/usr/bin/flatpak run …, /usr/bin/steam …) — match on the basename so
    // imported launchers keep their launcherId (Steam's Gamescope -e,
    // launcher-specific data setup)
    const QString execName = QFileInfo(firstToken).fileName();

    if (execName == QStringLiteral("flatpak")) {
        // Exported desktop entries insert options between "run" and the app
        // ID (--branch=…, --arch=…, --command …); the shared extractor skips
        // them — a fixed token position misses every exported launcher
        const QString appId = CommandVerifier::extractFlatpakAppId(command);
        if (appId == QStringLiteral("com.valvesoftware.Steam")) {
            return QStringLiteral("steam");
        }
        if (appId == QStringLiteral("com.heroicgameslauncher.hgl")) {
            return QStringLiteral("heroic");
        }
        if (appId == QStringLiteral("net.lutris.Lutris")) {
            return QStringLiteral("lutris");
        }
        return QString();
    }

    if (execName == QStringLiteral("steam")) {
        return QStringLiteral("steam");
    }
    if (execName == QStringLiteral("heroic")) {
        return QStringLiteral("heroic");
    }
    if (execName == QStringLiteral("lutris")) {
        return QStringLiteral("lutris");
    }

    return QString();
}

QString PresetManager::cleanExecCommand(const QString &exec)
{
    QString cleaned = exec;

    static const QStringList fieldCodes = {
        QStringLiteral("%f"), QStringLiteral("%F"),
        QStringLiteral("%u"), QStringLiteral("%U"),
        QStringLiteral("%d"), QStringLiteral("%D"),
        QStringLiteral("%n"), QStringLiteral("%N"),
        QStringLiteral("%i"),
        QStringLiteral("%c"),
        QStringLiteral("%k")
    };

    for (const QString &code : fieldCodes) {
        cleaned.remove(code);
    }

    cleaned = cleaned.simplified();

    return cleaned;
}

QString PresetManager::generateCustomId()
{
    return QStringLiteral("custom-") +
           QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

void PresetManager::saveBuiltinDataDirectoryOverride(const QString &id)
{
    // Built-in presets are rebuilt from detected defaults on every start, so
    // user edits only survive as persisted overrides (same "path|mode"
    // serialization as custom presets)
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Builtin Data Directories"));

    for (const LaunchPreset &preset : m_builtinPresets) {
        if (preset.id != id) {
            continue;
        }
        // An explicitly empty edit persists as an empty list, not as "default"
        group.writeEntry(id, encodeDataDirectories(preset.dataDirectories));
        group.sync();
        return;
    }
}

void PresetManager::applyBuiltinDataDirectoryOverrides()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    KConfigGroup group = config->group(QStringLiteral("Builtin Data Directories"));
    if (!group.exists()) {
        return;
    }

    for (int i = 0; i < m_builtinPresets.size(); ++i) {
        const QString id = m_builtinPresets[i].id;
        if (!group.hasKey(id)) {
            continue;
        }
        m_builtinPresets[i].dataDirectories = decodeDataDirectories(group.readEntry(id, QString()));
    }
}

void PresetManager::loadCustomPresets()
{
    m_customPresets.clear();

    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    const QStringList groups = config->groupList();

    static const QString prefix = QStringLiteral("Preset: ");

    for (const QString &groupName : groups) {
        if (!groupName.startsWith(prefix)) {
            continue;
        }

        KConfigGroup group = config->group(groupName);

        LaunchPreset preset;
        preset.id = group.readEntry(QStringLiteral("id"), QString());
        preset.name = group.readEntry(QStringLiteral("name"), QString());
        preset.command = group.readEntry(QStringLiteral("command"), QString());
        preset.workingDirectory = group.readEntry(QStringLiteral("workingDirectory"), QString());
        preset.iconName = group.readEntry(QStringLiteral("iconName"), QString());
        preset.desktopFilePath = group.readEntry(QStringLiteral("desktopFilePath"), QString());
        preset.isBuiltin = false;
        preset.flatpakAppId = group.readEntry(QStringLiteral("flatpakAppId"), QString());
        preset.flatpakArgs = group.readEntry(QStringLiteral("flatpakArgs"), QString());
        preset.launcherId = group.readEntry(QStringLiteral("launcherId"), QString());

        // JSON (new) with legacy "path|mode" line fallback; migrate legacy
        // sharedDirectories (plain path list) to bind-mode entries
        if (group.hasKey(QStringLiteral("dataDirectories"))) {
            preset.dataDirectories = decodeDataDirectories(group.readEntry(QStringLiteral("dataDirectories"), QString()));
            for (DataDirectory &dir : preset.dataDirectories) {
                if (dir.mode.isEmpty()) {
                    dir.mode = QStringLiteral("acl");
                }
            }
        } else if (group.hasKey(QStringLiteral("sharedDirectories"))) {
            const QStringList legacyDirs = group.readEntry(QStringLiteral("sharedDirectories"), QStringList());
            for (const QString &path : legacyDirs) {
                // Legacy sharedDirectories were bind-mounted at the player's
                // home-relative equivalent path — migrate as bind to preserve that
                preset.dataDirectories.append({path, QStringLiteral("bind")});
            }
            if (!legacyDirs.isEmpty()) {
                qCDebug(couchplayCore) << "Migrated legacy sharedDirectories for preset" << preset.id;
            }
        }

        // Migrate legacy steamIntegration key to launcherId
        if (preset.launcherId.isEmpty() && group.hasKey(QStringLiteral("steamIntegration"))
            && group.readEntry(QStringLiteral("steamIntegration"), false)) {
            preset.launcherId = QStringLiteral("steam");
            qCDebug(couchplayCore) << "Migrated legacy steamIntegration=true to launcherId=steam for preset" << preset.id;
        }

        if (!preset.id.isEmpty() && !preset.name.isEmpty()) {
            m_customPresets.append(preset);
        }
    }
}

void PresetManager::saveCustomPresets()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));

    static const QString prefix = QStringLiteral("Preset: ");

    QStringList existingGroups = config->groupList();
    for (const QString &groupName : existingGroups) {
        if (groupName.startsWith(prefix)) {
            config->deleteGroup(groupName);
        }
    }

    for (const LaunchPreset &preset : m_customPresets) {
        QString groupName = prefix + preset.id;
        KConfigGroup group = config->group(groupName);

        group.writeEntry(QStringLiteral("id"), preset.id);
        group.writeEntry(QStringLiteral("name"), preset.name);
        group.writeEntry(QStringLiteral("command"), preset.command);
        group.writeEntry(QStringLiteral("workingDirectory"), preset.workingDirectory);
        group.writeEntry(QStringLiteral("iconName"), preset.iconName);
        group.writeEntry(QStringLiteral("desktopFilePath"), preset.desktopFilePath);
        group.writeEntry(QStringLiteral("flatpakAppId"), preset.flatpakAppId);
        group.writeEntry(QStringLiteral("flatpakArgs"), preset.flatpakArgs);
        group.writeEntry(QStringLiteral("launcherId"), preset.launcherId);

        // JSON so paths containing '|' or newlines round-trip intact
        group.writeEntry(QStringLiteral("dataDirectories"), encodeDataDirectories(preset.dataDirectories));
    }

    config->sync();
}
