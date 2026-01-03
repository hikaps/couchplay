// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <qqmlintegration.h>

/**
 * @brief Manages PipeWire/PulseAudio configuration for multi-user audio
 */
class AudioManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool multiUserConfigured READ isMultiUserConfigured NOTIFY configurationChanged)
    Q_PROPERTY(QString audioServer READ audioServer CONSTANT)

public:
    explicit AudioManager(QObject *parent = nullptr);
    ~AudioManager() override;

    /**
     * @brief Check if multi-user audio is configured
     */
    bool isMultiUserConfigured() const { return m_multiUserConfigured; }

    /**
     * @brief Get the audio server type (pipewire or pulseaudio)
     */
    QString audioServer() const { return m_audioServer; }

    /**
     * @brief Check current configuration status
     */
    Q_INVOKABLE void checkConfiguration();

    /**
     * @brief Configure PipeWire for multi-user audio sharing
     * This enables TCP module for audio sharing between users
     * Requires helper for system configuration
     */
    Q_INVOKABLE bool configureMultiUser();

    /**
     * @brief Get the audio server address for secondary instances
     */
    Q_INVOKABLE QString getAudioServerAddress() const;

Q_SIGNALS:
    void configurationChanged();
    void errorOccurred(const QString &message);

private:
    bool m_multiUserConfigured = false;
    QString m_audioServer;
};
