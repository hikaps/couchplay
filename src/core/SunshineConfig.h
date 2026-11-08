// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

/**
 * @brief Everything Sunshine needs to stream one KWin output for one player
 */
struct StreamSpec {
    int instanceIndex = 0;
    QString identityKey; ///< Gaming user name; selects the persistent state directory
    QString outputName; ///< KWin output to capture (e.g. HDMI-A-1 or Virtual-couchplay-p2)
    QString sinkName; ///< PulseAudio sink Sunshine captures; empty = Sunshine default
    int port = 47989; ///< Sunshine base port (web UI is port + 1)
    int bitrate = 20000; ///< Maximum bitrate in kbps
    QString codec = QStringLiteral("auto"); ///< auto | h264 | hevc | av1
    QString displayName; ///< Name shown in Moonlight; empty = derived from index and identity
};

/**
 * @brief Generates per-player Sunshine configuration in a persistent per-user state directory
 *
 * Port offsets from base: -5 HTTP (legacy), +0 HTTP, +1 HTTPS web UI, +9 video UDP,
 * +10 control UDP, +11 audio UDP, +13 mic UDP, +21 RTSP. Instances spaced PORT_SPACING apart.
 */
class SunshineConfig
{
public:
    static constexpr int BASE_PORT = 47989;
    static constexpr int PORT_SPACING = 30;
    static constexpr int MIN_PORT = 1024;
    static constexpr int MAX_PORT = 65514;

    /** Name of the web UI / API account created by --creds. */
    static QString apiUser();

    /** Name Sunshine announces to Moonlight for @p spec. */
    static QString displayName(const StreamSpec &spec);

    /** Calculate the base port for an instance. */
    static int calculatePort(int instanceIndex, int basePort = BASE_PORT);

    /** State directory for an identity (not created): <AppData>/sunshine/<identityKey>. */
    static QString stateDirectory(const QString &identityKey);

    /** Create the state directory with mode 0700. Returns its path, or empty on failure. */
    static QString ensureStateDirectory(const QString &identityKey);

    /** Web UI password stored in <stateDir>/couchplay.secret (0600); created on first use. */
    static QString secret(const QString &stateDir);

    /**
     * Write <stateDir>/sunshine.conf and <stateDir>/apps.json.
     * @return Path to sunshine.conf, or empty on failure
     */
    static QString generateConfig(const StreamSpec &spec, const QString &stateDir);

    /** Arguments for `sunshine` that write the credentials file named in the conf. */
    static QStringList credentialArguments(const QString &confPath, const QString &secret);

    /**
     * Find a base port whose whole Sunshine port set is free.
     * Tries preferred, preferred + PORT_SPACING, ... up to @p attempts candidates.
     * @return The port, or -1 when none is free
     */
    static int allocatePort(int preferred, int attempts = 10);

    /**
     * Moonlight clients paired with this identity: [{name, uuid}] read from
     * <stateDir>/sunshine_state.json (root.named_devices). Empty when the file is missing or invalid.
     * Never creates the state directory.
     */
    static QVariantList pairedClients(const QString &identityKey);

    /** Number of entries pairedClients() returns. */
    static int pairedClientCount(const QString &identityKey);
};
