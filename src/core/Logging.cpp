// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "Logging.h"

// Define logging categories
// By default, debug messages are disabled; enable via QT_LOGGING_RULES

Q_LOGGING_CATEGORY(couchplayCore, "couchplay.core", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplaySteam, "couchplay.steam", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplayHelper, "couchplay.helper", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplayGamescope, "couchplay.gamescope", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplayDevices, "couchplay.devices", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplaySharing, "couchplay.sharing", QtWarningMsg)
