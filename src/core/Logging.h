// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QLoggingCategory>

/**
 * CouchPlay Logging Categories
 * 
 * Usage:
 *   #include "Logging.h"
 *   qCDebug(couchplaySteam) << "Message";
 *   qCWarning(couchplayCore) << "Warning message";
 * 
 * Enable via environment variable:
 *   QT_LOGGING_RULES="couchplay.*=true" ./build/bin/couchplay
 * 
 * Or use the run-debug.sh script:
 *   ./run-debug.sh
 */

// Core session management (SessionRunner, SessionManager)
Q_DECLARE_LOGGING_CATEGORY(couchplayCore)

// Steam configuration sync (SteamConfigManager)
Q_DECLARE_LOGGING_CATEGORY(couchplaySteam)

// D-Bus helper client communication
Q_DECLARE_LOGGING_CATEGORY(couchplayHelper)

// Gamescope instance management
Q_DECLARE_LOGGING_CATEGORY(couchplayGamescope)

// Device management (input devices)
Q_DECLARE_LOGGING_CATEGORY(couchplayDevices)

// Directory sharing
Q_DECLARE_LOGGING_CATEGORY(couchplaySharing)
