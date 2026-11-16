// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "SandboxCommand.h"

QStringList buildSandboxArgv(const SandboxSpec &spec)
{
    QStringList argv;
    argv << QStringLiteral("bwrap") << QStringLiteral("--unshare-user") << QStringLiteral("--uid")
         << QString::number(spec.uid) << QStringLiteral("--gid") << QString::number(spec.gid)
         << QStringLiteral("--unshare-pid") << QStringLiteral("--die-with-parent");

    // The host root stays visible, but /dev is rebuilt from scratch so only the devices bound below exist
    // (no /dev/uinput, no hidraw, no other players' input nodes - including ones that hotplug later).
    argv << QStringLiteral("--dev-bind") << QStringLiteral("/") << QStringLiteral("/") << QStringLiteral("--dev")
         << QStringLiteral("/dev") << QStringLiteral("--tmpfs") << QStringLiteral("/dev/shm");

    argv << QStringLiteral("--dev-bind") << QStringLiteral("/dev/dri") << QStringLiteral("/dev/dri");
    for (const QString &node : {QStringLiteral("/dev/snd"), QStringLiteral("/dev/fuse"), QStringLiteral("/dev/ntsync")}) {
        argv << QStringLiteral("--dev-bind-try") << node << node;
    }
    for (const QString &node : spec.extraDeviceNodes) {
        argv << QStringLiteral("--dev-bind") << node << node;
    }

    argv << QStringLiteral("--dir") << QStringLiteral("/dev/input");
    if (!spec.padNode.isEmpty()) {
        argv << QStringLiteral("--dev-bind") << spec.padNode << spec.padNode;
    }
    for (const QString &node : spec.directDevices) {
        argv << QStringLiteral("--dev-bind") << node << node;
    }

    argv << QStringLiteral("--proc") << QStringLiteral("/proc") << QStringLiteral("--tmpfs") << QStringLiteral("/tmp");

    if (!spec.playerHome.isEmpty()) {
        argv << QStringLiteral("--bind") << spec.playerHome << spec.home;
        // Sources resolve against the old root, so the host's per-user Flatpak installs stay reachable.
        const QString flatpakDir = spec.home + QStringLiteral("/.local/share/flatpak");
        argv << QStringLiteral("--ro-bind-try") << flatpakDir << flatpakDir;
    }

    for (const SandboxMount &mount : spec.mounts) {
        switch (mount.kind) {
        case SandboxMount::Bind:
            argv << QStringLiteral("--bind") << mount.source << mount.target;
            break;
        case SandboxMount::RoBind:
            argv << QStringLiteral("--ro-bind") << mount.source << mount.target;
            break;
        case SandboxMount::Overlay:
            argv << QStringLiteral("--overlay-src") << mount.source << QStringLiteral("--overlay") << mount.upper
                 << mount.work << mount.target;
            break;
        }
    }

    // Masking the session bus socket stops sandboxed code from reaching the host (flatpak-spawn --host).
    if (!spec.runtimeDir.isEmpty()) {
        argv << QStringLiteral("--ro-bind") << QStringLiteral("/dev/null")
             << spec.runtimeDir + QStringLiteral("/bus");
    }

    argv << QStringLiteral("--setenv") << QStringLiteral("HOME") << spec.home;
    for (auto it = spec.env.cbegin(); it != spec.env.cend(); ++it) {
        argv << QStringLiteral("--setenv") << it.key() << it.value();
    }
    argv << QStringLiteral("--setenv") << QStringLiteral("COUCHPLAY_SANDBOX_ID") << spec.token;
    argv << QStringLiteral("--unsetenv") << QStringLiteral("DBUS_SESSION_BUS_ADDRESS");

    argv << QStringLiteral("--chdir") << (spec.workingDirectory.isEmpty() ? spec.home : spec.workingDirectory);
    argv << QStringLiteral("--");

    argv << QStringLiteral("dbus-run-session") << QStringLiteral("--") << QStringLiteral("gamescope")
         << spec.gamescopeArgs;
    if (!spec.gameCommand.isEmpty()) {
        argv << QStringLiteral("--") << spec.gameCommand;
    }
    return argv;
}
