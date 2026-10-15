// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QString>

struct LaunchManifest;

namespace InstanceBootstrap
{
// Runs a trusted systemd launch manifest and replaces this process with Gamescope.
// Returns nonzero only on preparation or exec failure.
int run(const QString &token, const QString &helperExecutablePath);
// The helper supplies a validated manifest only after confirming unit termination.
bool cleanupCopyScratchDirectories(const LaunchManifest &manifest, const QString &launchToken,
                                   QString *error = nullptr);
}
