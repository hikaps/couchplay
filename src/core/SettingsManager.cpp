// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SettingsManager.h"

#include <QDebug>

#include <KSharedConfig>
#include <KConfigGroup>

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent)
{
    loadSettings();
}

void SettingsManager::loadSettings()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    
    // General settings
    KConfigGroup general = config->group(QStringLiteral("General"));
    m_hidePanels = general.readEntry(QStringLiteral("HidePanels"), true);
    m_killSteam = general.readEntry(QStringLiteral("KillSteam"), true);
    m_restoreSession = general.readEntry(QStringLiteral("RestoreSession"), false);
    m_ignoredDevices = general.readEntry(QStringLiteral("IgnoredDevices"), QStringList());
    
    // Gamescope settings
    KConfigGroup gamescope = config->group(QStringLiteral("Gamescope"));
    m_scalingMode = gamescope.readEntry(QStringLiteral("ScalingMode"), QStringLiteral("fit"));
    m_filterMode = gamescope.readEntry(QStringLiteral("FilterMode"), QStringLiteral("linear"));
    m_borderlessWindows = gamescope.readEntry(QStringLiteral("BorderlessWindows"), false);
    
    qDebug() << "SettingsManager: Loaded settings from couchplayrc";
}

void SettingsManager::saveAllSettings()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
    
    // General settings
    KConfigGroup general = config->group(QStringLiteral("General"));
    general.writeEntry(QStringLiteral("HidePanels"), m_hidePanels);
    general.writeEntry(QStringLiteral("KillSteam"), m_killSteam);
    general.writeEntry(QStringLiteral("RestoreSession"), m_restoreSession);
    general.writeEntry(QStringLiteral("IgnoredDevices"), m_ignoredDevices);
    
    // Gamescope settings
    KConfigGroup gamescope = config->group(QStringLiteral("Gamescope"));
    gamescope.writeEntry(QStringLiteral("ScalingMode"), m_scalingMode);
    gamescope.writeEntry(QStringLiteral("FilterMode"), m_filterMode);
    gamescope.writeEntry(QStringLiteral("BorderlessWindows"), m_borderlessWindows);
    
    config->sync();
}

void SettingsManager::setHidePanels(bool value)
{
    if (m_hidePanels != value) {
        m_hidePanels = value;
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("General")).writeEntry(QStringLiteral("HidePanels"), value);
        config->sync();
        Q_EMIT hidePanelsChanged();
    }
}

void SettingsManager::setKillSteam(bool value)
{
    if (m_killSteam != value) {
        m_killSteam = value;
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("General")).writeEntry(QStringLiteral("KillSteam"), value);
        config->sync();
        Q_EMIT killSteamChanged();
    }
}

void SettingsManager::setRestoreSession(bool value)
{
    if (m_restoreSession != value) {
        m_restoreSession = value;
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("General")).writeEntry(QStringLiteral("RestoreSession"), value);
        config->sync();
        Q_EMIT restoreSessionChanged();
    }
}

void SettingsManager::setScalingMode(const QString &value)
{
    if (m_scalingMode != value) {
        m_scalingMode = value;
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("Gamescope")).writeEntry(QStringLiteral("ScalingMode"), value);
        config->sync();
        Q_EMIT scalingModeChanged();
    }
}

void SettingsManager::setFilterMode(const QString &value)
{
    if (m_filterMode != value) {
        m_filterMode = value;
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("Gamescope")).writeEntry(QStringLiteral("FilterMode"), value);
        config->sync();
        Q_EMIT filterModeChanged();
    }
}

void SettingsManager::setBorderlessWindows(bool value)
{
    if (m_borderlessWindows != value) {
        m_borderlessWindows = value;
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("Gamescope")).writeEntry(QStringLiteral("BorderlessWindows"), value);
        config->sync();
        Q_EMIT borderlessWindowsChanged();
    }
}

void SettingsManager::setIgnoredDevices(const QStringList &value)
{
    if (m_ignoredDevices != value) {
        m_ignoredDevices = value;
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("General")).writeEntry(QStringLiteral("IgnoredDevices"), value);
        config->sync();
        Q_EMIT ignoredDevicesChanged();
    }
}

void SettingsManager::addIgnoredDevice(const QString &stableId)
{
    if (!m_ignoredDevices.contains(stableId)) {
        m_ignoredDevices.append(stableId);
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("General")).writeEntry(QStringLiteral("IgnoredDevices"), m_ignoredDevices);
        config->sync();
        Q_EMIT ignoredDevicesChanged();
    }
}

void SettingsManager::removeIgnoredDevice(const QString &stableId)
{
    if (m_ignoredDevices.contains(stableId)) {
        m_ignoredDevices.removeAll(stableId);
        KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("couchplayrc"));
        config->group(QStringLiteral("General")).writeEntry(QStringLiteral("IgnoredDevices"), m_ignoredDevices);
        config->sync();
        Q_EMIT ignoredDevicesChanged();
    }
}

void SettingsManager::resetToDefaults()
{
    m_hidePanels = true;
    m_killSteam = true;
    m_restoreSession = false;
    m_scalingMode = QStringLiteral("fit");
    m_filterMode = QStringLiteral("linear");
    m_borderlessWindows = false;
    m_ignoredDevices.clear();
    
    saveAllSettings();
    
    Q_EMIT hidePanelsChanged();
    Q_EMIT killSteamChanged();
    Q_EMIT restoreSessionChanged();
    Q_EMIT scalingModeChanged();
    Q_EMIT filterModeChanged();
    Q_EMIT borderlessWindowsChanged();
    Q_EMIT ignoredDevicesChanged();
    
    qDebug() << "SettingsManager: Reset all settings to defaults";
}
