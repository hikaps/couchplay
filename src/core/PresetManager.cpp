// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "PresetManager.h"
#include "HeroicConfigManager.h"
#include "Logging.h"
#include "SteamConfigManager.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

#include <KSharedConfig>
#include <KConfigGroup>

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

QStringList PresetManager::getDefaultSharedDirectories(const QString &id) const
{
    QStringList dirs;

    if (id == QStringLiteral("steam")) {
        if (m_steamConfigManager && m_steamConfigManager->isSteamDetected()) {
            QString steamRoot = m_steamConfigManager->steamPaths().steamRoot;
            if (!steamRoot.isEmpty()) {
                dirs.append(steamRoot);
            }
        }
    } else if (id == QStringLiteral("heroic")) {
        if (m_heroicConfigManager && m_heroicConfigManager->isHeroicDetected()) {
            // Don't share configPath: syncConfigToUser() copies specific config files to the gaming user's home,
            // and bind-mounting the config dir would cause ownership conflicts when syncing.
            // Only share the install path where games are installed.
            QString installPath = m_heroicConfigManager->defaultInstallPath();
            if (!installPath.isEmpty()) {
                dirs.append(installPath);
            }
        }
    } else if (id == QStringLiteral("lutris")) {
        QString home = QDir::homePath();
        QString lutrisData = home + QStringLiteral("/.local/share/lutris");
        QString lutrisGames = home + QStringLiteral("/Games");
        if (QDir(lutrisData).exists()) {
            dirs.append(lutrisData);
        }
        if (QDir(lutrisGames).exists()) {
            dirs.append(lutrisGames);
        }
    }

    dirs.removeDuplicates();
    return dirs;
}

void PresetManager::initBuiltinPresets()
{
    m_builtinPresets.clear();

    LaunchPreset steam;
    steam.id = QStringLiteral("steam");
    steam.name = QStringLiteral("Steam Big Picture");
    steam.command = QStringLiteral("steam -tenfoot -steamdeck");
    steam.iconName = QStringLiteral("steam");
    steam.isBuiltin = true;
    steam.steamIntegration = true;
    steam.launcherId = QStringLiteral("steam");
    steam.sharedDirectories = getDefaultSharedDirectories(QStringLiteral("steam"));
    m_builtinPresets.append(steam);

    LaunchPreset heroic;
    heroic.id = QStringLiteral("heroic");
    heroic.name = QStringLiteral("Heroic Games");
    heroic.iconName = QStringLiteral("com.heroicgameslauncher.hgl");
    heroic.isBuiltin = true;
    heroic.steamIntegration = false;
    heroic.launcherId = QStringLiteral("heroic");

        if (m_heroicConfigManager && m_heroicConfigManager->isHeroicDetected()) {
            heroic.command = m_heroicConfigManager->heroicCommand();
            heroic.launcherInfo.configPath = m_heroicConfigManager->configPath();
            heroic.launcherInfo.dataPath = m_heroicConfigManager->defaultInstallPath();
            heroic.launcherInfo.requiresAcls = true;
            heroic.launcherInfo.hasShortcutSync = true;

            if (m_heroicConfigManager->gameCount() == 0) {
                m_heroicConfigManager->loadGames();
            }
            heroic.launcherInfo.gameDirectories = m_heroicConfigManager->extractGameDirectories();
        } else {
            heroic.command = QStringLiteral("heroic");
        }
        heroic.sharedDirectories = getDefaultSharedDirectories(QStringLiteral("heroic"));
        m_builtinPresets.append(heroic);

    LaunchPreset lutris;
    lutris.id = QStringLiteral("lutris");
    lutris.name = QStringLiteral("Lutris");
    lutris.command = QStringLiteral("lutris");
    lutris.iconName = QStringLiteral("lutris");
    lutris.isBuiltin = true;
    lutris.steamIntegration = false;
    lutris.launcherId = QStringLiteral("lutris");
    lutris.sharedDirectories = getDefaultSharedDirectories(QStringLiteral("lutris"));
    m_builtinPresets.append(lutris);
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
        map[QStringLiteral("steamIntegration")] = preset.steamIntegration;
        map[QStringLiteral("launcherId")] = preset.launcherId;
        map[QStringLiteral("launcherInfo")] = QVariant::fromValue(preset.launcherInfo);
        map[QStringLiteral("sharedDirectories")] = preset.sharedDirectories;
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
        map[QStringLiteral("steamIntegration")] = app.steamIntegration;
        map[QStringLiteral("launcherId")] = app.launcherId;
        map[QStringLiteral("launcherInfo")] = QVariant::fromValue(app.launcherInfo);
        result.append(map);
    }
    return result;
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
            return preset;
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

bool PresetManager::getSteamIntegration(const QString &id) const
{
    return getPreset(id).steamIntegration;
}

QString PresetManager::getLauncherId(const QString &id) const
{
    return getPreset(id).launcherId;
}

QStringList PresetManager::getGameDirectories(const QString &id) const
{
    return getPreset(id).launcherInfo.gameDirectories;
}

QStringList PresetManager::getSharedDirectories(const QString &id) const
{
    return getPreset(id).sharedDirectories;
}

bool PresetManager::setSharedDirectories(const QString &id, const QStringList &directories)
{
    for (int i = 0; i < m_builtinPresets.size(); ++i) {
        if (m_builtinPresets[i].id == id) {
            m_builtinPresets[i].sharedDirectories = directories;
            Q_EMIT presetsChanged();
            return true;
        }
    }
    
    for (int i = 0; i < m_customPresets.size(); ++i) {
        if (m_customPresets[i].id == id) {
            m_customPresets[i].sharedDirectories = directories;
            saveCustomPresets();
            Q_EMIT presetsChanged();
            return true;
        }
    }

    qWarning() << "Cannot set shared directories - preset not found:" << id;
    return false;
}

QString PresetManager::addCustomPreset(const QString &name,
                                        const QString &command,
                                        const QString &workingDirectory,
                                        const QString &iconName,
                                        bool steamIntegration)
{
    LaunchPreset preset;
    preset.id = generateCustomId();
    preset.name = name;
    preset.command = command;
    preset.workingDirectory = workingDirectory;
    preset.iconName = iconName.isEmpty() ? QStringLiteral("application-x-executable") : iconName;
    preset.isBuiltin = false;
    preset.steamIntegration = steamIntegration;

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
                                        const QString &iconName,
                                        bool steamIntegration)
{
    for (int i = 0; i < m_customPresets.size(); ++i) {
        if (m_customPresets[i].id == id) {
            m_customPresets[i].name = name;
            m_customPresets[i].command = command;
            m_customPresets[i].workingDirectory = workingDirectory;
            m_customPresets[i].iconName = iconName;
            m_customPresets[i].steamIntegration = steamIntegration;

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

    // QString categories = desktop.value(QStringLiteral("Categories")).toString();

    return preset;
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
        preset.steamIntegration = group.readEntry(QStringLiteral("steamIntegration"), false);
        preset.sharedDirectories = group.readEntry(QStringLiteral("sharedDirectories"), QStringList());

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
        group.writeEntry(QStringLiteral("steamIntegration"), preset.steamIntegration);
        group.writeEntry(QStringLiteral("sharedDirectories"), preset.sharedDirectories);
    }

    config->sync();
}
