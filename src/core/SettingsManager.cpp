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
    m_steamIntegration = gamescope.readEntry(QStringLiteral("SteamIntegration"), true);
    m_borderlessWindows = gamescope.readEntry(QStringLiteral("BorderlessWindows"), false);
    
    qDebug() << "SettingsManager: Loaded settings from couchplayrc";
}

void SettingsManager::saveSettings()
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
    gamescope.writeEntry(QStringLiteral("SteamIntegration"), m_steamIntegration);
    gamescope.writeEntry(QStringLiteral("BorderlessWindows"), m_borderlessWindows);
    
    config->sync();
}

void SettingsManager::setHidePanels(bool value)
{
    if (m_hidePanels != value) {
        m_hidePanels = value;
        saveSettings();
        Q_EMIT hidePanelsChanged();
    }
}

void SettingsManager::setKillSteam(bool value)
{
    if (m_killSteam != value) {
        m_killSteam = value;
        saveSettings();
        Q_EMIT killSteamChanged();
    }
}

void SettingsManager::setRestoreSession(bool value)
{
    if (m_restoreSession != value) {
        m_restoreSession = value;
        saveSettings();
        Q_EMIT restoreSessionChanged();
    }
}

void SettingsManager::setScalingMode(const QString &value)
{
    if (m_scalingMode != value) {
        m_scalingMode = value;
        saveSettings();
        Q_EMIT scalingModeChanged();
    }
}

void SettingsManager::setFilterMode(const QString &value)
{
    if (m_filterMode != value) {
        m_filterMode = value;
        saveSettings();
        Q_EMIT filterModeChanged();
    }
}

void SettingsManager::setSteamIntegration(bool value)
{
    if (m_steamIntegration != value) {
        m_steamIntegration = value;
        saveSettings();
        Q_EMIT steamIntegrationChanged();
    }
}

void SettingsManager::setBorderlessWindows(bool value)
{
    if (m_borderlessWindows != value) {
        m_borderlessWindows = value;
        saveSettings();
        Q_EMIT borderlessWindowsChanged();
    }
}

void SettingsManager::setIgnoredDevices(const QStringList &value)
{
    if (m_ignoredDevices != value) {
        m_ignoredDevices = value;
        saveSettings();
        Q_EMIT ignoredDevicesChanged();
    }
}

void SettingsManager::addIgnoredDevice(const QString &stableId)
{
    if (!m_ignoredDevices.contains(stableId)) {
        m_ignoredDevices.append(stableId);
        saveSettings();
        Q_EMIT ignoredDevicesChanged();
    }
}

void SettingsManager::removeIgnoredDevice(const QString &stableId)
{
    if (m_ignoredDevices.contains(stableId)) {
        m_ignoredDevices.removeAll(stableId);
        saveSettings();
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
    m_steamIntegration = true;
    m_borderlessWindows = false;
    m_ignoredDevices.clear();
    
    saveSettings();
    
    Q_EMIT hidePanelsChanged();
    Q_EMIT killSteamChanged();
    Q_EMIT restoreSessionChanged();
    Q_EMIT scalingModeChanged();
    Q_EMIT filterModeChanged();
    Q_EMIT steamIntegrationChanged();
    Q_EMIT borderlessWindowsChanged();
    Q_EMIT ignoredDevicesChanged();
    
    qDebug() << "SettingsManager: Reset all settings to defaults";
}
