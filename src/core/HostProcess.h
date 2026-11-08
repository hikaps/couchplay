// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QString>
#include <QStringList>

class QProcess;

/**
 * @brief Runs programs as the desktop user on the host, from native or Flatpak builds
 *
 * Under Flatpak the process is started through `flatpak-spawn --host --watch-bus`, so it runs
 * unsandboxed as the desktop user and is killed when the app's bus connection goes away. Natively
 * it runs directly. KWin only offers the screencast protocol to unsandboxed clients, which is why
 * the virtual-display holder and Sunshine must be started this way.
 */
class HostProcess
{
public:
    static bool runningInFlatpak();

    /**
     * Configure (but don't start) @p process to run @p program with @p arguments on the host.
     * @param extraEnv "KEY=VALUE" entries added to the program's environment.
     */
    static void configure(QProcess &process,
                          const QString &program,
                          const QStringList &arguments,
                          const QStringList &extraEnv = {});

    /**
     * Absolute host path of a CouchPlay helper-side executable (e.g. "couchplay-vdisplay"),
     * or an empty string when it is not installed.
     */
    static QString hostLibexecPath(const QString &name);

    /** Whether @p path is an executable file on the host. */
    static bool hostExecutableExists(const QString &path);

    /** Forget cached lookups (tests). */
    static void clearCache();
};
