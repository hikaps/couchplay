// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

/** One extra mount inside a player's sandbox. */
struct SandboxMount {
    enum Kind {
        Bind, ///< read-write bind of @c source at @c target
        RoBind, ///< read-only bind of @c source at @c target
        Overlay ///< @c source as read-only lower layer; writes land in @c upper (with scratch dir @c work)
    };

    Kind kind = Bind;
    QString source;
    QString target;
    QString upper;
    QString work;
    QStringList lowerBelow; ///< Overlay only: further read-only layers under @c source, lowest first
};

/** Everything needed to build the `bwrap → dbus-run-session → gamescope` command line. */
struct SandboxSpec {
    QString home; ///< host $HOME (also the sandbox $HOME path)
    QString playerHome; ///< host directory bound at @c home; empty = the desktop user's own home (no bind)
    QString runtimeDir; ///< XDG_RUNTIME_DIR; its `bus` socket is masked
    QStringList maskedPaths; ///< host paths hidden behind /dev/null; the caller passes only ones that exist
    QString padNode; ///< the player's virtual pad (/dev/input/eventN); empty = none
    QStringList directDevices; ///< other input event nodes passed straight through
    QStringList extraDeviceNodes; ///< e.g. /dev/nvidia* present at launch
    QList<SandboxMount> mounts;
    QMap<QString, QString> env;
    QString token; ///< exported as COUCHPLAY_SANDBOX_ID so the host can find gamescope's PID
    QString workingDirectory;
    QStringList gamescopeArgs;
    QStringList gameCommand;
    uint uid = 0;
    uint gid = 0;
};

/** argv[0] is "bwrap". The result is passed to HostProcess::configure(). */
QStringList buildSandboxArgv(const SandboxSpec &spec);
