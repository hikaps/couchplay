// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QString>

// Polkit action IDs — must match data/polkit/io.github.hikaps.couchplay.policy
namespace PolkitActions {
inline const QString ACTION_DEVICE_OWNER = QStringLiteral("io.github.hikaps.couchplay.change-device-owner");
inline const QString ACTION_CREATE_USER = QStringLiteral("io.github.hikaps.couchplay.create-user");
inline const QString ACTION_DELETE_USER = QStringLiteral("io.github.hikaps.couchplay.delete-user");
inline const QString ACTION_ENABLE_LINGER = QStringLiteral("io.github.hikaps.couchplay.enable-linger");
inline const QString ACTION_WAYLAND_ACCESS = QStringLiteral("io.github.hikaps.couchplay.setup-wayland-access");
inline const QString ACTION_LAUNCH_INSTANCE = QStringLiteral("io.github.hikaps.couchplay.launch-instance");
inline const QString ACTION_MANAGE_MOUNTS = QStringLiteral("io.github.hikaps.couchplay.manage-mounts");
}
