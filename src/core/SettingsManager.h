// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <qqmlintegration.h>

/**
 * SettingsManager - Manages application settings persistence
 * 
 * Stores settings in ~/.config/couchplayrc using KSharedConfig.
 * All settings are automatically persisted when changed.
 */
class SettingsManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // General settings
    Q_PROPERTY(bool hidePanels READ hidePanels WRITE setHidePanels NOTIFY hidePanelsChanged)
    Q_PROPERTY(bool killSteam READ killSteam WRITE setKillSteam NOTIFY killSteamChanged)
    Q_PROPERTY(bool restoreSession READ restoreSession WRITE setRestoreSession NOTIFY restoreSessionChanged)

    // Gamescope settings
    Q_PROPERTY(QString scalingMode READ scalingMode WRITE setScalingMode NOTIFY scalingModeChanged)
    Q_PROPERTY(QString filterMode READ filterMode WRITE setFilterMode NOTIFY filterModeChanged)
    Q_PROPERTY(bool steamIntegration READ steamIntegration WRITE setSteamIntegration NOTIFY steamIntegrationChanged)
    Q_PROPERTY(bool borderlessWindows READ borderlessWindows WRITE setBorderlessWindows NOTIFY borderlessWindowsChanged)

public:
    explicit SettingsManager(QObject *parent = nullptr);
    ~SettingsManager() override = default;

    // General settings
    bool hidePanels() const { return m_hidePanels; }
    void setHidePanels(bool value);

    bool killSteam() const { return m_killSteam; }
    void setKillSteam(bool value);

    bool restoreSession() const { return m_restoreSession; }
    void setRestoreSession(bool value);

    // Gamescope settings
    QString scalingMode() const { return m_scalingMode; }
    void setScalingMode(const QString &value);

    QString filterMode() const { return m_filterMode; }
    void setFilterMode(const QString &value);

    bool steamIntegration() const { return m_steamIntegration; }
    void setSteamIntegration(bool value);

    bool borderlessWindows() const { return m_borderlessWindows; }
    void setBorderlessWindows(bool value);

    /**
     * Reset all settings to defaults
     */
    Q_INVOKABLE void resetToDefaults();

Q_SIGNALS:
    void hidePanelsChanged();
    void killSteamChanged();
    void restoreSessionChanged();
    void scalingModeChanged();
    void filterModeChanged();
    void steamIntegrationChanged();
    void borderlessWindowsChanged();

private:
    void loadSettings();
    void saveSettings();

    // General settings
    bool m_hidePanels = true;
    bool m_killSteam = true;
    bool m_restoreSession = false;

    // Gamescope settings
    QString m_scalingMode = QStringLiteral("fit");
    QString m_filterMode = QStringLiteral("linear");
    bool m_steamIntegration = true;
    bool m_borderlessWindows = false;
};
