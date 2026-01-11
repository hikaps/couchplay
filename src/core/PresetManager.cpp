// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "PresetManager.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

PresetManager::PresetManager(QObject *parent)
    : QObject(parent)
{
    initBuiltinPresets();
    loadCustomPresets();
}

PresetManager::~PresetManager() = default;

void PresetManager::initBuiltinPresets()
{
    m_builtinPresets.clear();

    // Steam Big Picture preset
    LaunchPreset steam;
    steam.id = QStringLiteral("steam");
    steam.name = QStringLiteral("Steam Big Picture");
    steam.command = QStringLiteral("steam -tenfoot -steamdeck");
    steam.iconName = QStringLiteral("steam");
    steam.isBuiltin = true;
    steam.steamIntegration = true;
    m_builtinPresets.append(steam);

    // Steam Desktop preset
    LaunchPreset steamDesktop;
    steamDesktop.id = QStringLiteral("steam-desktop");
    steamDesktop.name = QStringLiteral("Steam Desktop");
    steamDesktop.command = QStringLiteral("steam");
    steamDesktop.iconName = QStringLiteral("steam");
    steamDesktop.isBuiltin = true;
    steamDesktop.steamIntegration = true;
    m_builtinPresets.append(steamDesktop);

    // Lutris preset
    LaunchPreset lutris;
    lutris.id = QStringLiteral("lutris");
    lutris.name = QStringLiteral("Lutris");
    lutris.command = QStringLiteral("lutris");
    lutris.iconName = QStringLiteral("lutris");
    lutris.isBuiltin = true;
    lutris.steamIntegration = false;
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
    // Return default (Steam) if not found
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

    // Check if already exists
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

    // Standard .desktop file locations
    QStringList searchPaths = {
        QStringLiteral("/usr/share/applications"),
        QStringLiteral("/usr/local/share/applications"),
        QDir::homePath() + QStringLiteral("/.local/share/applications"),
        // Flatpak
        QDir::homePath() + QStringLiteral("/.local/share/flatpak/exports/share/applications"),
        QStringLiteral("/var/lib/flatpak/exports/share/applications"),
        // Snap
        QStringLiteral("/var/lib/snapd/desktop/applications")
    };

    QSet<QString> seenNames;  // Avoid duplicates by name

    for (const QString &searchPath : searchPaths) {
        QDir dir(searchPath);
        if (!dir.exists()) {
            continue;
        }

        const QStringList desktopFiles = dir.entryList({QStringLiteral("*.desktop")}, QDir::Files);
        for (const QString &fileName : desktopFiles) {
            QString filePath = dir.absoluteFilePath(fileName);
            LaunchPreset app = parseDesktopFile(filePath);

            // Skip invalid or already seen
            if (app.name.isEmpty() || seenNames.contains(app.name)) {
                continue;
            }

            // Skip if already added as a custom preset
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

    // Sort by name
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

    // Use QSettings to parse INI-like .desktop format
    QSettings desktop(filePath, QSettings::IniFormat);
    desktop.beginGroup(QStringLiteral("Desktop Entry"));

    // Check if it's an application
    QString type = desktop.value(QStringLiteral("Type")).toString();
    if (type != QStringLiteral("Application")) {
        return preset;
    }

    // Skip hidden entries
    if (desktop.value(QStringLiteral("Hidden"), false).toBool() ||
        desktop.value(QStringLiteral("NoDisplay"), false).toBool()) {
        return preset;
    }

    // Extract fields
    preset.name = desktop.value(QStringLiteral("Name")).toString();
    preset.command = cleanExecCommand(desktop.value(QStringLiteral("Exec")).toString());
    preset.workingDirectory = desktop.value(QStringLiteral("Path")).toString();
    preset.iconName = desktop.value(QStringLiteral("Icon")).toString();
    preset.desktopFilePath = filePath;

    // Check Categories for Game - could filter to only games in the future
    // QString categories = desktop.value(QStringLiteral("Categories")).toString();

    return preset;
}

QString PresetManager::cleanExecCommand(const QString &exec)
{
    QString cleaned = exec;

    // Remove freedesktop field codes
    // These are placeholders for file arguments, URLs, etc.
    static const QStringList fieldCodes = {
        QStringLiteral("%f"), QStringLiteral("%F"),  // File(s)
        QStringLiteral("%u"), QStringLiteral("%U"),  // URL(s)
        QStringLiteral("%d"), QStringLiteral("%D"),  // Directory (deprecated)
        QStringLiteral("%n"), QStringLiteral("%N"),  // Filename (deprecated)
        QStringLiteral("%i"),                         // Icon
        QStringLiteral("%c"),                         // Translated name
        QStringLiteral("%k")                          // Desktop file path
    };

    for (const QString &code : fieldCodes) {
        cleaned.remove(code);
    }

    // Clean up any double spaces and trim
    cleaned = cleaned.simplified();

    return cleaned;
}

QString PresetManager::generateCustomId()
{
    return QStringLiteral("custom-") + 
           QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString PresetManager::configFilePath() const
{
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return configDir + QStringLiteral("/couchplay/presets.json");
}

void PresetManager::loadCustomPresets()
{
    m_customPresets.clear();

    QString filePath = configFilePath();
    QFile file(filePath);

    if (!file.exists()) {
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open presets file:" << filePath;
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);

    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse presets JSON:" << error.errorString();
        return;
    }

    QJsonObject root = doc.object();
    QJsonArray presetsArray = root[QStringLiteral("customPresets")].toArray();

    for (const QJsonValue &value : presetsArray) {
        QJsonObject obj = value.toObject();

        LaunchPreset preset;
        preset.id = obj[QStringLiteral("id")].toString();
        preset.name = obj[QStringLiteral("name")].toString();
        preset.command = obj[QStringLiteral("command")].toString();
        preset.workingDirectory = obj[QStringLiteral("workingDirectory")].toString();
        preset.iconName = obj[QStringLiteral("iconName")].toString();
        preset.desktopFilePath = obj[QStringLiteral("desktopFilePath")].toString();
        preset.isBuiltin = false;
        preset.steamIntegration = obj[QStringLiteral("steamIntegration")].toBool();

        if (!preset.id.isEmpty() && !preset.name.isEmpty()) {
            m_customPresets.append(preset);
        }
    }
}

void PresetManager::saveCustomPresets()
{
    QString filePath = configFilePath();

    // Ensure directory exists
    QFileInfo fileInfo(filePath);
    QDir().mkpath(fileInfo.absolutePath());

    QJsonArray presetsArray;
    for (const LaunchPreset &preset : m_customPresets) {
        QJsonObject obj;
        obj[QStringLiteral("id")] = preset.id;
        obj[QStringLiteral("name")] = preset.name;
        obj[QStringLiteral("command")] = preset.command;
        obj[QStringLiteral("workingDirectory")] = preset.workingDirectory;
        obj[QStringLiteral("iconName")] = preset.iconName;
        obj[QStringLiteral("desktopFilePath")] = preset.desktopFilePath;
        obj[QStringLiteral("steamIntegration")] = preset.steamIntegration;
        presetsArray.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("customPresets")] = presetsArray;

    QJsonDocument doc(root);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to save presets file:" << filePath;
        Q_EMIT errorOccurred(QStringLiteral("Failed to save presets"));
        return;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
}
