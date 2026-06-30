// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <qqmlintegration.h>

/**
 * @brief Generates per-instance Sunshine streaming configuration
 *
 * Port offsets from base: +0 HTTP, +1 HTTPS, +9 video UDP, +10 control UDP,
 * +11 audio UDP, +21 RTSP. Instances spaced PORT_SPACING (30) apart.
 */
class SunshineConfig : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit SunshineConfig(QObject *parent = nullptr);
    ~SunshineConfig() override;

    static constexpr int BASE_PORT = 47989;
    static constexpr int PORT_SPACING = 30;
    static constexpr int MIN_PORT = 1024;
    static constexpr int MAX_PORT = 65514;

    /**
     * @brief Generate Sunshine config for a streaming instance
     *
     * Creates the config directory, writes sunshine.conf and apps.json.
     *
     * @param instanceConfig QVariantMap with instance settings.
     *   Supported keys: "username", "internalWidth", "internalHeight",
     *   "refreshRate", "outputName", "streamBitrate", "streamCodec",
     *   "sunshinePort" (overrides base port calculation).
     * @param instanceIndex 0-based index of this streaming instance
     * @param configDir Per-instance directory path (e.g. /tmp/couchplay-sunshine-0)
     * @return Path to the generated sunshine.conf, or empty string on failure
     */
    static QString generateConfig(const QVariantMap &instanceConfig, int instanceIndex, const QString &configDir);

    /**
     * @brief Calculate the base port for an instance
     * @param instanceIndex 0-based instance index
     * @param basePort Starting port (default BASE_PORT = 47989)
     * @return Calculated port for this instance
     */
    static int calculatePort(int instanceIndex, int basePort = BASE_PORT);

    /**
     * @brief Get the default config directory path for an instance
     * @param instanceIndex 0-based instance index
     * @return Path like /tmp/couchplay-sunshine-0
     */
    static QString defaultConfigDir(int instanceIndex);

private:
    static bool ensureConfigDirectory(const QString &configDir);
    static bool writeAppsJson(const QString &configDir);
    static bool writeCredentialsFile(const QString &configDir, const QString &username, const QString &password);
    static QString buildConfigContent(const QVariantMap &instanceConfig, int instanceIndex, const QString &configDir);
};
