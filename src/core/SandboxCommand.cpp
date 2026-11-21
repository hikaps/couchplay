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
            // bwrap stacks each --overlay-src above the previous one, so the lowest layers come first.
            for (const QString &lower : mount.lowerBelow) {
                argv << QStringLiteral("--overlay-src") << lower;
            }
            argv << QStringLiteral("--overlay-src") << mount.source << QStringLiteral("--overlay") << mount.upper
                 << mount.work << mount.target;
            break;
        }
    }

    // Masking the session bus and the systemd user manager's private socket stops sandboxed code from
    // reaching the host (flatpak-spawn --host, systemd-run --user).
    if (!spec.runtimeDir.isEmpty()) {
        for (const QString &socket : {QStringLiteral("/bus"), QStringLiteral("/systemd/private")}) {
            argv << QStringLiteral("--ro-bind") << QStringLiteral("/dev/null") << spec.runtimeDir + socket;
        }
    }

    // The host's system bus is masked too. Steam's web helper (Chromium) aborts when the system broker drops its
    // connection, which killed Steam at startup in a sizeable share of launches, and a game has no business calling
    // host services anyway. bwrap cannot create a missing mount point, so the caller lists only paths that exist.
    for (const QString &path : spec.maskedPaths) {
        argv << QStringLiteral("--ro-bind") << QStringLiteral("/dev/null") << path;
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
